// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "WorkspaceStructureFixture.h"

// test_remotesessioncore.cpp — the portable remote-session state machine,
// driven with fakes (no real transport, no daemon). Asserts the
// connect→Greet→ack→feed→reconnect→stop behavior, Normal Bye-vs-drop, the QUIC pre-ack
// fallback latch, and the reclaim handoff.

#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Session/ReconnectBackoff.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/RemoteSessionCore.h"

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/TerminalOperations.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Session;

namespace Ubiquitty::Session {

struct RemoteSessionWindowCloseAccess {
    // Fault injection for the direct-Core send contract. Normal teardown also
    // clears acknowledgement; leaving it set isolates a refused carrier write.
    static void removeCarrierWithoutStateChange(RemoteSessionCore& core)
    {
        core.m_channel.reset();
    }
};

struct RemoteSessionDatagramIngressAccess {
    [[nodiscard]] static constexpr std::size_t batchMaximumBytes()
    {
        return RemoteSessionCore::datagramIngressBatchMaximumBytes;
    }

    [[nodiscard]] static constexpr std::size_t batchMaximumFrames()
    {
        return RemoteSessionCore::datagramIngressBatchMaximumFrames;
    }

    [[nodiscard]] static constexpr std::size_t maxRetainedBytes()
    {
        return RemoteSessionCore::datagramIngressMaximumRetainedBytes;
    }

    [[nodiscard]] static constexpr std::size_t maxRetainedFrames()
    {
        return RemoteSessionCore::datagramIngressMaximumRetainedFrames;
    }

    [[nodiscard]] static constexpr std::size_t maxStorageBytes()
    {
        return sizeof(RemoteSessionCore::DatagramIngressBatch) * 2;
    }

    [[nodiscard]] static std::size_t pendingBytes(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        return core.m_datagramPending ? core.m_datagramPending->byteCount : 0;
    }

    [[nodiscard]] static std::size_t pendingFrames(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        return core.m_datagramPending ? core.m_datagramPending->frameCount : 0;
    }

    [[nodiscard]] static std::size_t retainedBytes(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        return (core.m_datagramPending ? core.m_datagramPending->byteCount : 0)
            + (core.m_datagramDraining ? core.m_datagramDraining->byteCount : 0);
    }

    [[nodiscard]] static std::size_t retainedFrames(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        return (core.m_datagramPending ? core.m_datagramPending->frameCount : 0)
            + (core.m_datagramDraining ? core.m_datagramDraining->frameCount : 0);
    }

    /// Takes the pending batch as the session thread does when a drain
    /// begins, and holds it there until `endDrain`.
    static void beginDrain(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        core.m_isDatagramDrainActive = true;
        core.m_datagramDraining.swap(core.m_datagramPending);
    }

    /// Releases the batch `beginDrain` took, unapplied.
    static void endDrain(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        if (core.m_datagramDraining)
            core.m_datagramDraining->clear();
        if (!core.m_datagramPending)
            core.m_datagramPending.swap(core.m_datagramDraining);
        core.m_isDatagramDrainActive = false;
    }

    [[nodiscard]] static std::size_t storageBytes(RemoteSessionCore& core)
    {
        const std::scoped_lock lock(core.m_inboundMutex);
        return (core.m_datagramPending ? sizeof(RemoteSessionCore::DatagramIngressBatch) : 0)
            + (core.m_datagramDraining ? sizeof(RemoteSessionCore::DatagramIngressBatch) : 0);
    }
};

} // namespace Ubiquitty::Session

namespace {
void check(bool ok, const char* what)
{
    CHECK_MSG(ok, what);
}

std::unique_ptr<Ubiquitty::Terminal> makeTerm() { return std::make_unique<Ubiquitty::Terminal>(); }

// Models the real async hop: `post` QUEUES (so a channel is never destroyed
// inside its own callback — teardown runs later, on pump()); `postDelayed`
// queues separately for the test to fire the backoff timer on demand.
class FakeDispatcher final : public Dispatcher {
public:
    void post(PostedWork work) override
    {
        ++m_postCount;
        m_queue.push_back(std::move(work));
    }
    void postDelayed(double seconds, PostedWork work) override
    {
        m_delaysRequested.push_back(seconds);
        m_delayed.push_back(std::move(work));
    }

    // Drain queued work (FIFO; a handler may post more — run those too).
    void pump()
    {
        while (!m_queue.empty()) {
            auto work = std::move(m_queue.front());
            m_queue.erase(m_queue.begin());
            std::move(work)();
        }
    }
    [[nodiscard]] bool hasDelayed() const { return !m_delayed.empty(); }
    // The backoff is only observable as the delay the core asked for, so the
    // schedule is asserted from this rather than from a wall clock.
    [[nodiscard]] const std::vector<double>& delaysRequested() const { return m_delaysRequested; }
    [[nodiscard]] std::size_t postCount() const { return m_postCount; }
    [[nodiscard]] std::size_t queuedCount() const { return m_queue.size(); }
    [[nodiscard]] PostedWork takeNext()
    {
        if (m_queue.empty()) {
            return {};
        }
        auto work = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        return work;
    }
    void fireDelayed()
    {
        auto delayed = std::move(m_delayed);
        m_delayed.clear();
        for (auto& w : delayed)
            m_queue.push_back(std::move(w));
        pump();
    }

private:
    std::vector<PostedWork> m_queue;
    std::vector<PostedWork> m_delayed;
    std::vector<double> m_delaysRequested;
    std::size_t m_postCount = 0;
};

using FrameLog = std::vector<std::vector<std::uint8_t>>;

// Whether a fake carrier takes the datagrams the core hands it.
enum class DatagramSupport : std::uint8_t {
    Refused,
    Accepted,
};

// Records outbound frames into a SHARED log owned by the factory — so they
// survive this channel's teardown (stop()/a drop destroys the channel). Lets
// the test simulate connect / data / close.
class FakeChannel final : public Ubiquitty::Transport::Channel {
public:
    // `liveSlot` is the factory's own `last()` pointer. The factory hands
    // ownership to the core and keeps a raw pointer to watch it with, so the
    // channel has to say when it dies — otherwise last() names freed memory the
    // moment the core drops its channel, which every test here then reads.
    FakeChannel(Transport::Origin origin, FrameLog* sink, FakeChannel** liveSlot = nullptr)
        : m_origin(std::move(origin))
        , m_sink(sink)
        , m_liveSlot(liveSlot)
    {
    }
    ~FakeChannel() override
    {
        // Only if the slot still names this channel: a later channel may have
        // taken it already, and clearing that would hide the live one.
        if (m_liveSlot && *m_liveSlot == this)
            *m_liveSlot = nullptr;
    }
    bool start(Ubiquitty::Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        // A platform channel that cannot start fires no callback at all, so
        // the core must notice the false return rather than wait forever.
        return !m_startFails;
    }
    void setStartFails(bool fails) { m_startFails = fails; }
    void send(std::span<const std::uint8_t> data) override
    {
        if (m_sink)
            m_sink->emplace_back(data.begin(), data.end());
    }
    void stop() override { }
    // How many times the core told the carrier its interface was gone.
    void repath() override { ++m_repaths; }
    [[nodiscard]] int repaths() const { return m_repaths; }
    // Whether the carrier takes datagrams, as a QUIC one that negotiated them
    // does; a stream carrier refuses every one.
    void setDatagramSupport(DatagramSupport support) { m_datagramSupport = support; }
    bool sendDatagram(std::span<const std::uint8_t> datagram) override
    {
        if (m_datagramSupport == DatagramSupport::Refused)
            return false;
        m_sentDatagrams.emplace_back(datagram.begin(), datagram.end());
        return true;
    }
    // The datagrams the core sent, in order.
    [[nodiscard]] const std::vector<std::vector<std::uint8_t>>& sentDatagrams() const { return m_sentDatagrams; }
    // How much of what was sent is still queued. The base class answers -1 —
    // "cannot say" — which is what a channel without a queue of its own has to
    // report, so the test drives both that and a real depth.
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override { return m_pendingOutbound; }
    void setPendingOutboundBytes(std::int64_t pending) { m_pendingOutbound = pending; }
    // What the carrier will hold before it refuses or ends. -1 by default,
    // the same "cannot say" the base class gives, so the unset case writes as
    // a carrier with no stated capacity does.
    [[nodiscard]] std::int64_t outboundCapacityBytes() const override { return m_outboundCapacity; }
    void setOutboundCapacityBytes(std::int64_t capacity) { m_outboundCapacity = capacity; }
    [[nodiscard]] Transport::Origin origin() const override { return m_origin; }
    [[nodiscard]] Ubiquitty::Transport::Health health() const override { return m_health; }
    [[nodiscard]] std::string description() const override { return "fake"; }
    // The application error code the host's close carries, as a QUIC
    // CONNECTION_CLOSE does; none by default, as a stream's EOF has none.
    [[nodiscard]] std::optional<std::uint64_t> peerApplicationCloseCode() const override { return m_applicationCloseCode; }
    void setApplicationCloseCode(std::uint64_t code) { m_applicationCloseCode = code; }

    // Test drivers (call the observer as the I/O thread would).
    void fireConnect()
    {
        m_health = Ubiquitty::Transport::Health::Connected;
        if (m_observer)
            m_observer->onConnect();
    }
    void fireData(const std::vector<std::uint8_t>& bytes)
    {
        if (m_observer)
            m_observer->onData(bytes);
    }
    void fireClose(const std::string& reason)
    {
        m_health = Ubiquitty::Transport::Health::Closed;
        if (m_observer)
            m_observer->onClose(reason);
    }
    // The unreliable path: QUIC delivers frames as datagrams beside the
    // reliable stream, and the core marshals them onto the session thread the
    // same way. Nothing drove it before, so the whole ingress was dark.
    void fireDatagram(const std::vector<std::uint8_t>& bytes)
    {
        if (m_observer)
            m_observer->onDatagram(bytes);
    }

private:
    bool m_startFails = false;
    DatagramSupport m_datagramSupport = DatagramSupport::Refused;
    std::vector<std::vector<std::uint8_t>> m_sentDatagrams;
    int m_repaths = 0;
    std::int64_t m_pendingOutbound = -1;
    std::int64_t m_outboundCapacity { -1 };
    std::optional<std::uint64_t> m_applicationCloseCode;
    Transport::Origin m_origin;
    FrameLog* m_sink = nullptr;
    FakeChannel** m_liveSlot = nullptr;
    Ubiquitty::Transport::ChannelObserver* m_observer = nullptr;
    Ubiquitty::Transport::Health m_health = Ubiquitty::Transport::Health::Connecting;
};

// Hands the core a fresh FakeChannel each attempt; records preferQUIC and a
// persistent log of every frame any channel sent (survives channel teardown).
class FakeFactory final : public ChannelFactory {
public:
    void createChannel(TransportMode preferQUIC, Done done) override
    {
        m_connectionBegin = m_sentLog.size();
        ++m_createCount;
        m_lastCommand = substituteReattachSocket(remoteBootstrapCommand(), m_daemonSocket);
        m_lastPreferQUIC = (preferQUIC == TransportMode::QUIC);
        m_creationFailure = std::exchange(m_nextCreationFailure, std::nullopt);
        if (m_creationFailure) {
            m_last = nullptr;
            done(nullptr, m_offerQUIC);
            return;
        }
        if (m_buildFails) {
            // The documented "the platform could not build one" answer.
            m_last = nullptr;
            done(nullptr, m_offerQUIC);
            return;
        }
        auto channel = std::make_unique<FakeChannel>(Transport::Origin { Transport::OriginKind::Daemon, "" }, &m_sentLog, &m_last);
        channel->setStartFails(m_startFails);
        m_last = channel.get();
        done(std::move(channel), m_offerQUIC);
    }
    void setBuildFails(bool fails) { m_buildFails = fails; }
    void setStartFails(bool fails) { m_startFails = fails; }
    /// Makes the next attempt build nothing, for the reason given, as a
    /// bootstrap that reported why does.
    void failNextCreation(ChannelCreationFailure failure) { m_nextCreationFailure = std::move(failure); }
    [[nodiscard]] std::optional<ChannelCreationFailure> creationFailure() const override { return m_creationFailure; }
    [[nodiscard]] FakeChannel* last() const { return m_last; }
    [[nodiscard]] int createCount() const { return m_createCount; }
    [[nodiscard]] bool lastPreferQUIC() const { return m_lastPreferQUIC; }
    void setOfferQUIC(bool quic) { m_offerQUIC = quic; }
    void setQUICFallbackReason(std::string reason) { m_quicFallbackReason = std::move(reason); }
    [[nodiscard]] std::string quicFallbackReason() const override { return m_quicFallbackReason; }
    bool pinDaemonSocket(std::string_view socket) override
    {
        if (!m_daemonSocket.empty() && socket != m_daemonSocket)
            return false;
        m_daemonSocket = socket;
        return true;
    }
    void clearHandoverReach() override
    {
        m_daemonSocket.clear();
        ++m_handoverClears;
    }
    [[nodiscard]] const std::string& lastCommand() const { return m_lastCommand; }
    [[nodiscard]] int handoverClears() const { return m_handoverClears; }

    [[nodiscard]] std::size_t sentCount() const { return m_sentLog.size(); }
    /// Every frame any channel sent, in order.
    [[nodiscard]] const FrameLog& sentLog() const { return m_sentLog; }
    [[nodiscard]] std::span<const std::vector<std::uint8_t>> connectionWrites() const { return std::span { m_sentLog }.subspan(m_connectionBegin); }
    /// How many direct envelopes of a kind went out, across write boundaries.
    [[nodiscard]] std::size_t sentScreenFramesOfKind(Protocol::Sync::FrameKind kind) const
    {
        return sentCountOfType(kind);
    }
    [[nodiscard]] bool sentAnyOfType(Protocol::Sync::FrameKind type) const
    {
        return sentCountOfType(type) != 0;
    }
    [[nodiscard]] std::size_t sentCountOfType(Protocol::Sync::FrameKind type) const
    {
        std::size_t count = 0;
        Protocol::Sync::FrameDecoder decoder;
        for (const auto& bytes : m_sentLog) {
            decoder.append(bytes);
            while (const auto frame = decoder.next()) {
                if (frame->kind == type)
                    ++count;
            }
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None);
        CHECK(decoder.bufferedBytes() == 0);
        return count;
    }

    /// An owned body, so the decoder's next envelope cannot invalidate it.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> nthOfType(Protocol::Sync::FrameKind type, std::size_t index) const
    {
        Protocol::Sync::FrameDecoder decoder;
        for (const auto& bytes : m_sentLog) {
            decoder.append(bytes);
            while (const auto frame = decoder.next()) {
                if (frame->kind == type && index-- == 0)
                    return std::vector<std::uint8_t>(frame->body.begin(), frame->body.end());
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> firstOfType(Protocol::Sync::FrameKind type) const
    {
        return nthOfType(type, 0);
    }

    /// Every `Viewport` the client sent, in order: its size of the pane, as
    /// the direct stream carries it across however many carrier writes.
    [[nodiscard]] std::vector<Protocol::Sync::Viewport> sentViewports() const
    {
        Protocol::Sync::FrameDecoder decoder;
        std::vector<Protocol::Sync::Viewport> viewports;
        for (const auto& bytes : m_sentLog) {
            decoder.append(bytes);
            while (const auto frame = decoder.next()) {
                if (frame->kind == Protocol::Sync::FrameKind::Viewport) {
                    if (const auto viewport = Protocol::Sync::decodeViewport(frame->body))
                        viewports.push_back(*viewport);
                }
            }
        }
        return viewports;
    }

    /// Every write belongs to the current direct envelope grammar, not an
    /// obsolete outer operation. Counts also enforce complete framing.
    [[nodiscard]] bool hasCanonicalTranscript() const
    {
        Protocol::Sync::FrameDecoder decoder;
        for (const auto& bytes : m_sentLog) {
            decoder.append(bytes);
            while (const auto frame = decoder.next()) {
                if (Protocol::Sync::directionOf(frame->kind) == Protocol::Sync::Direction::HostToClient)
                    return false;
            }
            if (decoder.error() != Protocol::Sync::EnvelopeError::None)
                return false;
        }
        return decoder.bufferedBytes() == 0;
    }

    [[nodiscard]] std::size_t sentCalls(std::string_view method) const
    {
        std::size_t count = 0;
        for (std::size_t index = 0;; ++index) {
            const auto body = nthOfType(Protocol::Sync::FrameKind::Call, index);
            if (!body)
                break;
            const auto call = Protocol::Sync::decodeCall(*body);
            CHECK(call);
            if (call) {
                const auto request = Protocol::Sync::decodeCallRequest(call->text);
                CHECK(request.outcome == Protocol::Control::RequestOutcome::Dispatch);
                if (request.method == method)
                    ++count;
            }
        }
        return count;
    }

    /// The size the most recent `Viewport` named, or nothing when none went out.
    [[nodiscard]] std::optional<Protocol::Sync::Viewport> lastViewport() const
    {
        const std::vector<Protocol::Sync::Viewport> viewports = sentViewports();
        return viewports.empty() ? std::nullopt : std::optional { viewports.back() };
    }

private:
    std::optional<ChannelCreationFailure> m_nextCreationFailure;
    std::optional<ChannelCreationFailure> m_creationFailure;
    std::size_t m_connectionBegin = 0;
    bool m_buildFails = false;
    bool m_startFails = false;
    FakeChannel* m_last = nullptr;
    int m_createCount = 0;
    bool m_lastPreferQUIC = false;
    bool m_offerQUIC = false;
    std::string m_quicFallbackReason;
    std::string m_daemonSocket;
    std::string m_lastCommand;
    int m_handoverClears = 0;
    FrameLog m_sentLog;
};

class RecordingObserver final : public RemoteSessionObserver {
public:
    void onStateChanged(State state, const std::string& reason) override
    {
        m_state = state;
        m_lastReason = reason;
        ++m_stateChanges;
        if (state == State::Failed)
            m_failureReasons.push_back(reason);
    }
    void onScreenChanged() override
    {
        ++m_screens;
        if (m_onScreen)
            m_onScreen();
    }
    void onTitle(std::string_view utf8) override
    {
        ++m_titles;
        m_orderedEvents.emplace_back(utf8);
    }
    void onBell() override
    {
        ++m_bells;
        m_orderedEvents.emplace_back("bell");
        if (m_onBell) {
            m_onBell();
        }
    }
    void onExit() override { ++m_exits; }
    void onCommandDone(const Protocol::Sync::EffectView& effect, const Protocol::Sync::CommandFinishedEffect& command, Protocol::Sync::EffectDisposition disposition) override
    {
        ++m_commandDones;
        m_lastExitCode = command.exitStatus;
        m_lastCurrentDirectory = command.currentDirectory;
        m_lastCommandGeneration = effect.generation;
        m_lastCommandSequence = effect.sequence;
        m_lastCommandDurationMicroseconds = command.durationMicroseconds;
        m_lastCommandDisposition = disposition;
        m_orderedEvents.push_back("command " + std::to_string(command.exitStatus));
    }
    void onSessionEstablished(uint32_t sessionID) override
    {
        m_sessionID = sessionID;
        ++m_established;
    }
    void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& result) override
    {
        m_workspaceResults.push_back(result);
    }
    void onSessionList(std::uint32_t requestID, const std::vector<Ubiquitty::Session::SessionRow>& rows) override
    {
        m_directoryAnswers.push_back(requestID);
        m_directoryRows = rows;
        if (m_onDirectory)
            m_onDirectory();
    }
    void onSessionListFailed(std::uint32_t requestID) override
    {
        m_directoryFailures.push_back(requestID);
    }
    void onFileData(Protocol::Control::FileReadReply&& reply) override
    {
        m_files.push_back(std::move(reply));
        if (m_onFile)
            m_onFile();
    }
    void onPastePushAck(const Protocol::Control::PastePushReply& reply) override
    {
        m_pastes.push_back(reply);
        if (m_onPaste)
            m_onPaste();
    }
    void onClipboardRequest(const Protocol::Sync::ClipboardRequestMessage& request) override
    {
        ++m_clipboardRequests;
        m_lastClipboardRequest = request;
    }
    void onClipboardRoutingChanged() override { ++m_clipboardRoutingChanges; }
    void onReclaimed() override { ++m_reclaims; }
    void onDeployRequested(const std::string& platformArch) override
    {
        m_deployPlatform = platformArch;
        ++m_deploys;
    }

    /// How many clipboard requests the host made, and the last of them.
    [[nodiscard]] int clipboardRequests() const { return m_clipboardRequests; }
    [[nodiscard]] const Protocol::Sync::ClipboardRequestMessage& lastClipboardRequest() const { return m_lastClipboardRequest; }

    State m_state = State::Idle;
    std::string m_lastReason;
    int m_stateChanges = 0;
    std::vector<std::string> m_failureReasons;
    int m_screens = 0;
    int m_titles = 0;
    int m_bells = 0;
    int m_exits = 0;
    int m_established = 0;
    int m_clipboardRequests = 0;
    int m_clipboardRoutingChanges = 0;
    Protocol::Sync::ClipboardRequestMessage m_lastClipboardRequest;
    int m_reclaims = 0;
    int m_deploys = 0;
    int m_commandDones = 0;
    std::uint64_t m_lastCommandGeneration = 0;
    std::uint64_t m_lastCommandSequence = 0;
    std::uint64_t m_lastCommandDurationMicroseconds = 0;
    Protocol::Sync::EffectDisposition m_lastCommandDisposition = Protocol::Sync::EffectDisposition::Ignore;
    std::string m_deployPlatform;
    uint32_t m_sessionID = 0;
    int32_t m_lastExitCode = 0;
    std::string m_lastCurrentDirectory;
    std::vector<std::string> m_orderedEvents;
    std::vector<Ubiquitty::Session::WorkspaceIntentResult> m_workspaceResults;
    std::vector<Protocol::Control::FileReadReply> m_files;
    std::function<void()> m_onFile;
    std::vector<Protocol::Control::PastePushReply> m_pastes;
    std::function<void()> m_onPaste;
    std::vector<std::uint32_t> m_directoryAnswers;
    std::vector<std::uint32_t> m_directoryFailures;
    std::vector<Ubiquitty::Session::SessionRow> m_directoryRows;
    std::function<void()> m_onDirectory;
    std::function<void()> m_onBell;
    std::function<void()> m_onScreen;
};

/// Admits the fixture's held session, without binding its pane yet. Selection
/// comes from the fixture, never from an arbitrary requested numeric identity.
std::vector<std::uint8_t> welcomeOnly(UbiquittyTest::HostScreen& host, const FakeFactory& factory,
    std::uint32_t sessionID, std::uint64_t daemonInstanceID = 17, std::uint32_t workspaceID = 0, std::string socketName = "fixture.sock")
{
    namespace Sync = Protocol::Sync;
    auto& connection = host.plane().connection();
    CHECK(connection.state() == Sync::ConnectionState::Greeting);
    if (connection.state() != Sync::ConnectionState::Greeting)
        return {};
    connection.setGreetingAdmission([sessionID, daemonInstanceID, workspaceID, socketName = std::move(socketName)](const Sync::Greet& greet) -> std::expected<Sync::HostIdentity, Sync::Bye> {
        const auto& request = greet.request;
        if (request.target != Sync::AdmissionTarget::Session
            || (request.sessionID && request.sessionID != sessionID)
            || request.workspaceID
            || (request.expectedDaemonInstanceID && request.expectedDaemonInstanceID != daemonInstanceID))
            return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone });
        return Sync::HostIdentity { .mayDrive = true, .verifiedKind = Sync::ClientKind::Human, .selection = { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .workspaceID = workspaceID, .daemonInstanceID = daemonInstanceID, .socketName = socketName, .hostPlatform = "fixture" } };
    });
    for (const auto& bytes : factory.connectionWrites())
        connection.receiveStream(bytes, host.plane().now());
    host.passOver(factory.sentLog().size());
    CHECK(connection.state() == Sync::ConnectionState::Attached);
    if (connection.state() != Sync::ConnectionState::Attached)
        return {};
    const auto reply = host.flush();
    Sync::FrameDecoder decoder;
    decoder.append(reply);
    std::size_t welcomes = 0;
    while (const auto frame = decoder.next()) {
        welcomes += frame->kind == Sync::FrameKind::Welcome;
        CHECK(frame->kind != Sync::FrameKind::PaneBind);
    }
    CHECK(welcomes == 1);
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    return reply;
}

/// The actual terminal and screen plane serving one held fixture session.
class SessionHost {
public:
    /// - Parameters:
    ///   - sessionID: The session the core's Welcome names.
    ///   - daemonInstanceID: The daemon instance it names.
    ///   - rows: The host terminal's rows.
    ///   - columns: Its columns.
    ///   - datagramMaxBytes: The largest datagram the carrier sends, 0 for
    ///     none.
    SessionHost(std::uint32_t sessionID, std::uint64_t daemonInstanceID, int rows, int columns, std::size_t datagramMaxBytes = 0)
        : m_screen(sessionID, daemonInstanceID, rows, columns, datagramMaxBytes)
        , m_sessionID(sessionID)
        , m_daemonInstanceID(daemonInstanceID)
    {
    }

    /// The host's terminal, which a case writes to between publishes.
    Ubiquitty::Terminal& terminal() { return m_screen.terminal(); }

    std::vector<std::uint8_t> welcome(const FakeFactory& factory, std::string socketName = "fixture.sock")
    {
        return welcomeOnly(m_screen, factory, m_sessionID, m_daemonInstanceID, 0, std::move(socketName));
    }

    /// Reads what the core sent since the last call, then returns the frames
    /// that publish the host's terminal as it now stands.
    std::vector<std::uint8_t> publish(const FakeFactory& factory)
    {
        m_screen.receive(factory.sentLog());
        return m_screen.publish();
    }

    /// Moves the host's grid, marks its first cell so the frame differs from
    /// the last, and publishes.
    std::vector<std::uint8_t> publishAt(const FakeFactory& factory, int rows, int columns)
    {
        Ubiquitty::resize(m_screen.terminal(), rows, columns, Ubiquitty::Modes::ReflowMode::Fixed);
        m_mark = m_mark == 'a' ? 'b' : 'a';
        const char cell[] = { '\x1b', '[', 'H', m_mark, 0 };
        Ubiquitty::processBytes(&m_screen.terminal(), cell);
        return publish(factory);
    }

    /// Publishes the host's terminal as it now stands without reading what
    /// the core sent since: what the host had already written when it reads
    /// the core's next frame.
    std::vector<std::uint8_t> publishUnread() { return m_screen.publish(); }

    /// Takes the datagrams the host has queued.
    std::vector<std::vector<std::uint8_t>> takeDatagrams() { return m_screen.plane().takeDatagrams(); }

    /// Moves the host clock on, reads what the core sent since the last call
    /// on the stream and as datagrams, and returns what the host then sends
    /// on the stream, publishing nothing.
    ///
    /// - Parameters:
    ///   - factory: The core's channels, whose stream the host reads.
    ///   - datagrams: The datagrams the core sent, read in order.
    ///   - microseconds: How far the host clock moves first.
    std::vector<std::uint8_t> idle(const FakeFactory& factory, std::span<const std::vector<std::uint8_t>> datagrams, std::uint64_t microseconds)
    {
        m_screen.plane().advanceClock(microseconds);
        m_screen.receive(factory.sentLog());
        for (const std::vector<std::uint8_t>& datagram : datagrams)
            m_screen.plane().receiveDatagram(datagram);
        return m_screen.flush();
    }

    /// The host's end of the screen plane.
    Ubiquitty::Protocol::Sync::HostConnection& connection() { return m_screen.plane().connection(); }

    /// The handle the host bound the pane to.
    [[nodiscard]] std::uint32_t paneHandle() { return m_screen.plane().handle(); }

    /// The pane the host serves.
    Ubiquitty::Protocol::Sync::PaneSource& source() { return m_screen.plane().source(); }

    /// What the core's input came to at the pane's program.
    [[nodiscard]] const UbiquittyTest::TypedInput& typed() { return m_screen.plane().typed(); }

    /// The host clock.
    [[nodiscard]] std::uint64_t now() { return m_screen.plane().now(); }

    /// Leaves what the core sent so far to an earlier connection's host.
    void passOver(const FakeFactory& factory) { m_screen.passOver(factory.sentLog().size()); }

private:
    UbiquittyTest::HostScreen m_screen;
    std::uint32_t m_sessionID;
    std::uint64_t m_daemonInstanceID;
    char m_mark = 'a';
};

// ---- inbound frame builders (host → client) ----
std::vector<std::uint8_t> helloAckFrame(uint32_t sessionID, std::string_view platform = "fixture")
{
    Protocol::Sync::Welcome welcome;
    welcome.wireVersion = Protocol::Sync::wireVersion;
    welcome.hostInstanceID = { 17, 0 };
    welcome.attachmentID = { 1, 17 };
    welcome.selection = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = sessionID, .daemonInstanceID = 17, .socketName = "fixture.sock", .hostPlatform = std::string(platform) };
    std::vector<std::uint8_t> body, out;
    Protocol::Sync::encode(body, welcome);
    CHECK(Protocol::Sync::appendFrame(out, Protocol::Sync::FrameKind::Welcome, 0, body));
    return out;
}
std::vector<std::uint8_t> authDeniedFrame(Protocol::Sync::ByeReason reason)
{
    std::vector<std::uint8_t> body, out;
    Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = reason });
    CHECK(Protocol::Sync::appendFrame(out, Protocol::Sync::FrameKind::Bye, 0, body));
    return out;
}
std::vector<std::uint8_t> bellFrame(UbiquittyTest::HostScreen& host, const FakeFactory& factory)
{
    host.receive(factory.sentLog());
    CHECK(host.plane().source().appendEffect(Protocol::Sync::BellEffect {}, host.plane().connection().clientInstance()));
    return host.flush();
}
std::vector<std::uint8_t> clipboardRequestFrame(UbiquittyTest::HostScreen& host, const FakeFactory& factory, const Protocol::Sync::ClipboardRead& request)
{
    host.receive(factory.sentLog());
    CHECK(host.plane().connection().requestClipboard(host.plane().handle(), request));
    return host.flush();
}
std::vector<std::uint8_t> titleFrame(std::string_view title, std::uint32_t handle = 1)
{
    Protocol::Sync::PaneFacts facts;
    facts.revision = 1;
    facts.programTitle = title;
    std::vector<std::uint8_t> body;
    Protocol::Sync::encodePaneFacts(body, facts);
    std::vector<std::uint8_t> envelope;
    CHECK(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::PaneFacts, handle, body));
    return envelope;
}

std::vector<std::uint8_t> sizedDatagramFrame(std::size_t totalBytes)
{
    // Queue-only malformed datagrams are never drained. Exact byte sizes test
    // transport storage admission independently of the pane-body parser.
    CHECK(totalBytes >= 3);
    std::vector<std::uint8_t> out(totalBytes, 0);
    out[0] = std::to_underlying(Protocol::Sync::FrameKind::Echo);
    return out;
}
std::vector<std::uint8_t> exitFrame()
{
    std::vector<std::uint8_t> body, out;
    Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = Protocol::Sync::ByeReason::Normal });
    CHECK(Protocol::Sync::appendFrame(out, Protocol::Sync::FrameKind::Bye, 0, body));
    return out;
}
std::vector<std::uint8_t> reclaimReleasedFrame(const Protocol::Control::ReclaimTarget& target, std::string_view correlation)
{
    const auto id = Codec::JSON::Value::string(std::string(correlation));
    Protocol::Sync::Result result;
    result.text = Codec::JSON::serialize(Protocol::Control::reclaimReleasedResult(&id, target));
    std::vector<std::uint8_t> body, envelope;
    Protocol::Sync::encode(body, result);
    CHECK(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Result, 0, body));
    return envelope;
}
// A canonical foreground record; the client mirrors isAtPrompt to gate reflow.
std::vector<std::uint8_t> statusFrame(bool foregroundIsShell, const char* process)
{
    Protocol::Sync::PaneFacts facts;
    facts.revision = foregroundIsShell ? 3 : 2;
    facts.isAtPrompt = foregroundIsShell;
    facts.foreground.name = process;
    std::vector<std::uint8_t> body;
    Protocol::Sync::encodePaneFacts(body, facts);
    std::vector<std::uint8_t> envelope;
    CHECK(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::PaneFacts, 1, body));
    return envelope;
}

// The raw bytes the bootstrap prints on its stdout (this transport's pipe) when
// the host has no daemon binary — sniffed pre-handshake by the core.
std::vector<std::uint8_t> sentinelBytes(const char* literal) { return { literal, literal + std::strlen(literal) }; }

constexpr auto helloType = Protocol::Sync::FrameKind::Greet;
constexpr auto exitType = Protocol::Sync::FrameKind::Bye;

// Fire a channel callback, then drain the dispatcher so the marshaled handler
// runs OUTSIDE the channel callback (modeling the real async hop).
void pumpConnect(FakeChannel* ch, FakeDispatcher& d)
{
    ch->fireConnect();
    d.pump();
}
void pumpData(FakeChannel* ch, FakeDispatcher& d, const std::vector<std::uint8_t>& f)
{
    ch->fireData(f);
    d.pump();
}
void pumpClose(FakeChannel* ch, FakeDispatcher& d, const std::string& r)
{
    ch->fireClose(r);
    d.pump();
}
// Fire a close callback without draining — the marshaled handler stays queued.
// Models the I/O thread posting onClose before the session thread has run the
// work already queued ahead of it: when that earlier work tears the channel
// down (bumping the generation), this stale-generation close is dropped on the
// later pump and the now-destroyed channel is never touched again.
void queueClose(FakeChannel* ch, const std::string& r) { ch->fireClose(r); }

/// A core attached to session 7 whose screen plane a host welcomed: the
/// session a wake or a path change can ask for a proof of life. A connection
/// whose plane is not welcomed cannot be asked, and is replaced unasked.
struct WelcomedSession {
    std::unique_ptr<Ubiquitty::Terminal> term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    std::optional<RemoteSessionCore> core;
    std::optional<SessionHost> host;
    std::string socketName;
    std::uint64_t daemonInstance = 17;

    /// - Parameter carrier: The carrier the factory builds.
    explicit WelcomedSession(TransportMode carrier, std::string socket = "fixture.sock", std::uint64_t instance = 17)
        : socketName(std::move(socket))
        , daemonInstance(instance)
    {
        factory.setOfferQUIC(carrier == TransportMode::QUIC);
        REQUIRE(factory.pinDaemonSocket(socketName));
        core.emplace(term.get(), &dispatcher, &factory, &observer);
        core->start(0, 80, 24);
        attach();
    }

    /// Completes the handshake on the carrier the core last dialled, with a
    /// host of its own that welcomes the screen plane and binds the pane.
    void attach()
    {
        host.emplace(7, daemonInstance, 24, 80);
        host->passOver(factory);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, host->welcome(factory, socketName));
        pumpData(factory.last(), dispatcher, host->publish(factory));
        pumpData(factory.last(), dispatcher, host->publish(factory));
    }

    /// How many writes led with a screen-plane `Probe`.
    [[nodiscard]] std::size_t probes() const { return factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::Probe); }

    /// Hands the core what the host says to everything the core sent since:
    /// its `Echo` of a probe among it.
    void answer() { pumpData(factory.last(), dispatcher, host->idle(factory, {}, 0)); }

    /// Hands the core a frame of the screen the host wrote without having
    /// read what the core sent since, as output queued before a probe is.
    ///
    /// - Parameter text: What the host's program printed.
    void deliverUnreadOutput(std::string_view text)
    {
        Ubiquitty::processBytes(&host->terminal(), text);
        pumpData(factory.last(), dispatcher, host->publishUnread());
    }
};

// ---- tests ----
TEST(aCommandAccessoryFollowsTheWelcomedHostPlatform)
{
    check(isMacOSHostPlatform("Darwin arm64"), "a Darwin host offers the Command accessory");
    check(!isMacOSHostPlatform("Linux aarch64"), "a Linux host offers no Command accessory");
    check(!isMacOSHostPlatform("Darwin "), "an incomplete platform offers no Command accessory");

    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    check(!core.hasMacOSHost(), "no host has answered before Welcome");
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(1, "Darwin arm64"));
    check(core.hasMacOSHost(), "the accepted Welcome offers the Command accessory");
    term.reset();
}

TEST(test_connect_hello_ack)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

    core.start(0, 80, 24);
    check(factory.createCount() == 1, "start creates one channel");
    check(observer.m_state == State::Connecting, "state Connecting after start");
    check(!core.acknowledged(), "not acknowledged before Welcome");
    check(factory.last() && factory.sentCount() == 0, "no Greet before transport connect");

    pumpConnect(factory.last(), dispatcher);
    check(factory.sentAnyOfType(helloType), "Greet sent on transport connect");

    pumpData(factory.last(), dispatcher, helloAckFrame(4242));
    check(core.state() == State::Connected, "state Connected after Welcome");
    check(core.sessionID() == 4242, "sessionID pinned from Welcome");
    check(core.acknowledged(), "acknowledged after Welcome");
    check(observer.m_established == 1 && observer.m_sessionID == 4242, "onSessionEstablished fired");

    term.reset();
}

TEST(anUnreadableWelcomeReportsItsDecodeFailure)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    Protocol::Sync::FrameDecoder decoder;
    decoder.append(helloAckFrame(4242));
    const auto welcome = decoder.next();
    REQUIRE(welcome && !welcome->body.empty());
    std::vector<std::uint8_t> truncated;
    REQUIRE(Protocol::Sync::appendFrame(truncated, Protocol::Sync::FrameKind::Welcome, 0, welcome->body.first(welcome->body.size() - 1)));
    pumpData(factory.last(), dispatcher, truncated);

    CHECK(core.state() == State::Reconnecting);
    CHECK_MSG(observer.m_lastReason.find("an unreadable Welcome") != std::string::npos, observer.m_lastReason.c_str());
}

TEST(aFrameBeforeWelcomeNamesTheAdmissionFailure)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, sentinelBytes("\nUBIQUITTY-SYNC\n"));
    std::vector<std::uint8_t> probe;
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, Protocol::Sync::Probe { .time = 1 });
    REQUIRE(Protocol::Sync::appendFrame(probe, Protocol::Sync::FrameKind::Probe, 0, body));
    pumpData(factory.last(), dispatcher, probe);
    CHECK(core.state() == State::Reconnecting);
    CHECK_MSG(observer.m_lastReason.find("a Sync frame before Welcome") != std::string::npos, observer.m_lastReason.c_str());
}

/// A Welcome this client refuses ends the read it arrived in.
///
/// The host's next frames are often in the same read: a refused daemon's screen
/// must not be painted into the mirror, its title must not reach the window,
/// and the session reports one failure, with the reason it was refused.
TEST(aRefusedHelloAckDecodesNothingAfterIt)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setExpectedDaemonInstanceID(5);
    core.start(9, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    const int screensBefore = observer.m_screens;

    SessionHost host(9, 17, 24, 80);
    Ubiquitty::processBytes(&host.terminal(), "REFUSED DAEMON");
    // The actual host emits one Welcome selecting instance 17, not the pin 5.
    std::vector<std::uint8_t> chunk = host.publish(factory);
    const std::vector<std::uint8_t> title = titleFrame("wrong");
    chunk.insert(chunk.end(), title.begin(), title.end());
    check(!chunk.empty(), "the refused host published its screen");
    pumpData(factory.last(), dispatcher, chunk);

    check(core.state() == State::Failed, "the refused host failed the session");
    check(observer.m_titles == 0, "the refused host's title reached the window");
    check(observer.m_screens == screensBefore, "a repaint was announced after the refusal");
    check(term->rows != 24 || term->columns != 80
            || Ubiquitty::Screen::visibleCell(term.get(), 0, 0, 0) == nullptr
            || Ubiquitty::Screen::visibleCell(term.get(), 0, 0, 0)->codePoint != 'R',
        "the refused host's screen was painted into the mirror");
    check(observer.m_failureReasons.size() == 1, "the session reported one failure");
    check(!observer.m_failureReasons.empty() && observer.m_failureReasons.back() != "detached",
        "the failure names why the host was refused");
}

/// The clipboard a remote host does not have: the request arrives, the platform
/// answers from its own pasteboard, and the typed answer reaches that request.
///
/// This is the direction the rest of the protocol does not run. Every other
/// reply on this client is the host answering the client; here the HOST asks,
/// because the thing being asked for only exists on the near side.
TEST(sessionServesTheClipboardItsHostCannotHave)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));

    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());

    // The host asks which types are held. The probe carries no MIME, because
    // asking "which" and asking "for one" are different questions.
    pumpData(factory.last(), dispatcher, clipboardRequestFrame(host, factory, { .requestID = 41 }));

    check(observer.clipboardRequests() == 1, "the host's clipboard request reached the platform");
    check(observer.lastClipboardRequest().requestID == 41, "and carried the id the answer must echo");
    check(observer.lastClipboardRequest().kind == Protocol::Sync::ClipboardRequestKind::Types,
        "and said it wanted the type list");

    // The platform answers the exact pending host request.
    const std::size_t before = factory.sentCount();
    core.serveClipboard(41, Protocol::Sync::ClipboardServeStatus::OK,
        { { "image/png", {} }, { "text/plain", {} } });
    check(factory.sentCount() > before, "answering sent a frame");

    host.receive(factory.sentLog());
    unsigned replies = 0;
    CHECK(host.plane().connection().drainClipboardReplies([&](uint32_t handle, const auto& request, auto status, const auto& bundle) {
        ++replies;
        CHECK(handle == host.plane().handle());
        CHECK(request.requestID == 41);
        CHECK(status == Protocol::Sync::EffectClipboardStatus::OK);
        CHECK(bundle.count == 2);
        CHECK(bundle.items[0].mime == "image/png");
        CHECK(bundle.items[1].mime == "text/plain");
        return true;
    }));
    CHECK(replies == 1);
}

/// A payload past the clipboard bound is refused as TooLarge rather than
/// truncated, because half a PNG is worse than a clipboard a program reads as
/// unavailable — the program can act on the second and cannot detect the first.
TEST(sessionRefusesAClipboardPayloadPastTheFrame)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));

    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    pumpData(factory.last(), dispatcher, clipboardRequestFrame(host, factory, { .requestID = 9, .kind = Protocol::Sync::EffectClipboardRequestKind::Item, .mime = "image/png" }));

    std::vector<Protocol::Sync::ClipboardServeItem> huge;
    huge.push_back({ "image/png", std::vector<std::uint8_t>(Protocol::Sync::maxClipboardServeBytes + 1, 0x7F) });
    core.serveClipboard(9, Protocol::Sync::ClipboardServeStatus::OK, huge);

    host.receive(factory.sentLog());
    unsigned replies = 0;
    CHECK(host.plane().connection().drainClipboardReplies([&](uint32_t handle, const auto& request, auto status, const auto& bundle) {
        ++replies;
        CHECK(handle == host.plane().handle());
        CHECK(request.requestID == 9);
        CHECK(status == Protocol::Sync::EffectClipboardStatus::TooLarge);
        CHECK(bundle.count == 0);
        return true;
    }));
    CHECK(replies == 1);
}

/// A refusal carries no items even when the caller passes some: a frame that
/// said Refused and carried bytes would be two answers, and a reader would have
/// to pick one.
TEST(sessionsRefusalCarriesNothing)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));

    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    pumpData(factory.last(), dispatcher, clipboardRequestFrame(host, factory, { .requestID = 3, .kind = Protocol::Sync::EffectClipboardRequestKind::Item, .mime = "text/plain" }));

    core.serveClipboard(3, Protocol::Sync::ClipboardServeStatus::Refused, { { "text/plain", { 'n', 'o' } } });

    host.receive(factory.sentLog());
    unsigned replies = 0;
    CHECK(host.plane().connection().drainClipboardReplies([&](uint32_t handle, const auto& request, auto status, const auto& bundle) {
        ++replies;
        CHECK(handle == host.plane().handle());
        CHECK(request.requestID == 3);
        CHECK(status == Protocol::Sync::EffectClipboardStatus::Refused);
        CHECK(bundle.count == 0);
        return true;
    }));
    CHECK(replies == 1);
}

/// Before the handshake there is nobody to answer, and a frame written then
/// would reach a host that has not agreed what the session is.
TEST(sessionServesNoClipboardBeforeTheHandshake)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    // No Welcome.

    const auto before = factory.sentCount();
    core.serveClipboard(1, Protocol::Sync::ClipboardServeStatus::OK, { { "text/plain", { 'x' } } });
    check(factory.sentCount() == before,
        "nothing is served before the host has acknowledged the session");
}

/// A client that was not told to serve does not claim that it will.
///
/// The default matters more than the opt-in here: claiming this hands every
/// process on the host a way to read what the user copied on the machine they
/// are sitting at, and an offer that arrives by default is one nobody chose.
TEST(aClientClaimsNoClipboardServingUnlessItIsTurnedOn)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    const auto hello = factory.firstOfType(helloType);
    REQUIRE(hello);
    const auto decodedHelloMessage = Protocol::Sync::decodeGreet(*hello);
    check(decodedHelloMessage.has_value(), "and it decodes as a Greet");
    const Protocol::Sync::Greet& decoded
        = decodedHelloMessage.value_or(Protocol::Sync::Greet {});

    check(!decoded.request.servesClipboard,
        "a client nobody asked does not offer this machine's clipboard to the host");
    check(!core.didClaimClipboardServe(), "and it reports the claim it did not make");
}

/// The exact admission request in the first Greet a fresh core sends.
///
/// - Parameters:
///   - policy: What the core is told to ask when the named session ended.
///   - attachSessionID: The session the core starts on, 0 for a fresh one.
/// - Returns: The decoded request, or nothing if no valid Greet was sent.
std::optional<Protocol::Sync::AdmissionRequest> helloSessionFlagsFor(ReattachPolicy policy, std::uint32_t attachSessionID)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setReattachPolicy(policy);
    core.start(attachSessionID, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    const auto hello = factory.firstOfType(helloType);
    check(hello.has_value(), "a Greet was sent");
    if (!hello)
        return std::nullopt;
    const auto decoded = Protocol::Sync::decodeGreet(*hello);
    check(decoded.has_value(), "and it decodes as a Greet");
    return decoded ? std::optional { decoded->request } : std::nullopt;
}

/// A connect returning to a remembered session asks to attach or be refused.
///
/// A saved numeric identity must never create a replacement shell. Only a
/// request naming no session can ask for a new one, under either facade policy.
TEST(aRememberedSessionHelloAsksToAttachOnly)
{
    for (const auto policy : { ReattachPolicy::AttachOnly, ReattachPolicy::AttachOrCreate }) {
        const auto held = helloSessionFlagsFor(policy, 42);
        REQUIRE(held);
        CHECK(held->target == Protocol::Sync::AdmissionTarget::Session);
        CHECK(held->sessionID == 42 && held->isAttachOnly);
        const auto fresh = helloSessionFlagsFor(policy, 0);
        REQUIRE(fresh);
        CHECK(fresh->sessionID == 0 && !fresh->isAttachOnly);
    }
}

/// Turned on, the client claims it and says so.
///
/// A frontend has to read the accessor rather than the setting, because the
/// setting can be on while the connection in hand claimed nothing — and a
/// badge saying otherwise would name a disclosure that is not happening.
TEST(aClientTurnedOnClaimsClipboardServingAndReportsIt)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setClipboardServing(Clipboard::Serving::Enabled);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    const auto hello = factory.firstOfType(helloType);
    REQUIRE(hello);
    const auto decodedHelloMessage = Protocol::Sync::decodeGreet(*hello);
    check(decodedHelloMessage.has_value(), "and it decodes as a Greet");
    const Protocol::Sync::Greet& decoded
        = decodedHelloMessage.value_or(Protocol::Sync::Greet {});

    check(decoded.request.servesClipboard,
        "the client offers to answer the host's clipboard requests");
    check(core.didClaimClipboardServe(), "and reports the claim it made");
}

/// The claim latches into the Greet, so a setting flipped mid-session does not
/// reach the connection in hand.
///
/// This is the whole reason ``Clipboard::ScopeFacts`` carries the setting and
/// the claim as separate facts. The host recorded what a Greet said; a client
/// that started answering because a preference changed would be replying to a
/// question the host does not believe it asked, and a badge that went orange on
/// the setting alone would name a disclosure that is not happening.
TEST(aSettingTurnedOnMidSessionDoesNotReachTheHelloAlreadySent)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(!core.didClaimClipboardServe(), "the session opened without the claim");

    core.setClipboardServing(Clipboard::Serving::Enabled);
    check(!core.didClaimClipboardServe(),
        "and turning the setting on does not retroactively claim it on this connection");
}

TEST(test_input_resize_gated_on_ack)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    const std::size_t beforeAck = factory.sentCount();
    core.sendInput("x");
    core.setSize(100, 40);
    check(factory.sentCount() == beforeAck, "input/resize are no-ops before Welcome");
    check(!core.send(Protocol::Sync::RawInput { .bytes = { 'x' } }), "input was taken before the host named a session");

    SessionHost host(1, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    const std::size_t rawBefore = factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::Raw);
    const std::uint64_t echoesBefore = core.trace().latency().count;
    core.sendInput("hello");
    check(factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::Raw) == rawBefore + 1, "input did not leave as a Raw event after the pane was bound");
    // The trace times an echo from the input's leaving to the next screen.
    Ubiquitty::processBytes(&host.terminal(), "hello");
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(core.trace().latency().count == echoesBefore + 1, "input that left opened no echo-latency sample for the screen after it to close");
    core.setSize(120, 50);
    const auto asked = factory.lastViewport();
    check(asked && asked->columns == 120 && asked->rows == 50, "a size change after the pane was bound sent no Viewport naming it");

    term.reset();
}

// The host has no window, so the pixels a thin client draws its grid in are the
// only pixels its session has: the program reads them as ws_xpixel/ws_ypixel, and
// a program that places pictures refuses a terminal that reports none. They ride
// the pane's Viewport: published before the handshake, they go out once the host
// has bound the pane; published after, a change goes out on a Viewport naming the
// grid already held, and a repeat sends nothing.
TEST(test_the_host_is_told_the_pixels_the_client_draws_in)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setPixelGeometry({ { 9, 18 }, { 720, 432 }, { 740, 450 } });
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(1, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    check(factory.sentViewports().empty(), "a Viewport left for a pane the host has not bound");
    pumpData(factory.last(), dispatcher, host.publish(factory));

    const auto first = factory.lastViewport();
    check(first.has_value(), "the bind sent no Viewport carrying the pixels published before the handshake");
    if (first) {
        check(first->columns == 80 && first->rows == 24, "the pixels' Viewport moved the grid");
        check(first->cellPixelWidth == 9 && first->cellPixelHeight == 18, "the Viewport carries the published cell");
        check(first->textAreaPixelWidth == 720 && first->textAreaPixelHeight == 432, "the Viewport carries the published text area");
        check(first->drawablePixelWidth == 740 && first->drawablePixelHeight == 450, "the Viewport carries the published drawable");
    }
    pumpData(factory.last(), dispatcher, host.publish(factory));
    const std::vector<Protocol::Sync::ViewportReport> heard = host.connection().takeViewportReports();
    check(heard.size() == 1 && first && heard[0].viewport == *first, "the host did not read the client's Viewport as it was sent");

    const std::size_t afterBind = factory.sentViewports().size();
    core.setPixelGeometry({ { 9, 18 }, { 720, 432 }, { 740, 450 } });
    core.setWindowSize(80, 24, 720, 432);
    check(factory.sentViewports().size() == afterBind, "pixels the host already has were sent again");

    core.setPixelGeometry({ { 18, 36 }, { 1440, 864 }, { 1480, 900 } });
    core.setWindowSize(80, 24, 1440, 864);
    const auto rescaled = factory.lastViewport();
    check(factory.sentViewports().size() == afterBind + 1 && rescaled && rescaled->cellPixelWidth == 18 && rescaled->columns == 80,
        "a new scale did not reach the host on a Viewport that keeps the grid");

    core.setSize(100, 30);
    const auto regridded = factory.lastViewport();
    check(regridded && regridded->columns == 100 && regridded->textAreaPixelWidth == 1440, "a grid ask dropped the pixels");

    term.reset();
}

TEST(fileReadsUseOneCorrelatedTypedCallOnTheBoundPane)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    CHECK(core.requestFile("/srv/not-welcomed") == 0);
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    const auto requestID = core.requestFile("/srv/report.txt");
    REQUIRE(requestID);
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    CHECK(factory.hasCanonicalTranscript());
    REQUIRE(calls.size() == 1);
    bool valid = false; // bool-ok: parser output
    const auto call = Codec::JSON::parse(calls.front().text, valid);
    REQUIRE(valid && call.find("params"));
    CHECK(call.stringOr("method") == "file.read");
    CHECK(call.find("params")->numberOr("pane") == 1);
    CHECK(call.find("params")->stringOr("path") == "/srv/report.txt");
}

TEST(fileReadMovesOnlyCorrelatedBytesAndRetiresBeforeCallbackDestruction)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    auto core = std::make_unique<RemoteSessionCore>(term.get(), &dispatcher, &factory, &observer);
    core->start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    const auto requestID = core->requestFile("/srv/file");
    REQUIRE(requestID);
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool valid = false;
    const auto request = Codec::JSON::parse(calls.front().text, valid);
    REQUIRE(valid);
    CHECK(host.plane().connection().answerCall(calls.front().callIndex,
              R"({"jsonrpc":"2.0","id":")" + request.stringOr("id")
                  + R"(","result":{"pane":1,"path":"/srv/file","status":"ok","bytes":3,"data":"AP9B"}})")
        == Protocol::Sync::CallAnswer::Sent);
    observer.m_onFile = [&] { core.reset(); };
    pumpData(factory.last(), dispatcher, host.flush());
    CHECK(!core);
    REQUIRE(observer.m_files.size() == 1);
    CHECK(observer.m_files.front().requestID == requestID);
    CHECK(observer.m_files.front().status == Protocol::Control::FileReadStatus::OK);
    CHECK(std::ranges::equal(observer.m_files.front().data(), std::array<std::uint8_t, 3> { 0, 255, 'A' }));
}

TEST(fileReadRejectsWrongPanePathOrLengthAndBoundsPendingCalls)
{
    for (const auto* result : {
             R"({"pane":2,"path":"/srv/file","status":"ok","bytes":3,"data":"YWJj"})",
             R"({"pane":1,"path":"/srv/other","status":"ok","bytes":3,"data":"YWJj"})",
             R"({"pane":1,"path":"/srv/file","status":"ok","bytes":4,"data":"YWJj"})",
             R"({"pane":1,"path":"/srv/file","status":"denied","bytes":3,"data":"YWJj"})" }) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        UbiquittyTest::HostScreen host(1, 17);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.publish());
        for (unsigned index = 0; index < 16; ++index)
            REQUIRE(core.requestFile("/srv/file"));
        CHECK(core.requestFile("/srv/file") == 0);
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 16);
        bool valid = false;
        const auto request = Codec::JSON::parse(calls.front().text, valid);
        REQUIRE(valid);
        CHECK(host.plane().connection().answerCall(calls.front().callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.flush());
        REQUIRE(observer.m_files.size() == 1);
        CHECK(observer.m_files.front().status == Protocol::Control::FileReadStatus::Denied);
        CHECK(observer.m_files.front().data().empty());
        CHECK(core.requestFile("/srv/file") != 0);
    }
}

TEST(retiredFileDataCannotDeliverUnsolicitedBytes)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(1));
    // Outer tag 0x9A is not a canonical frame kind. A plausible FILE_DATA
    // length/body behind it must not deliver unsolicited file bytes.
    const std::vector<std::uint8_t> frame { 0x9A, 1, 0, 0, 0, 'a' };
    pumpData(factory.last(), dispatcher, frame);
    CHECK(core.state() == State::Reconnecting);
    CHECK(factory.last() == nullptr);
    CHECK(factory.createCount() == 1);
    CHECK(observer.m_files.empty());
}

TEST(directoryRequestsRetireOnCompletionAndPermitCallbackDestruction)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    auto core = std::make_unique<RemoteSessionCore>(term.get(), &dispatcher, &factory, &observer);
    core->start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    CHECK(core->requestSessionList() == 0);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    const auto requestID = core->requestSessionList();
    REQUIRE(requestID);
    CHECK(core->requestSessionList() == 0);
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false; // bool-ok: the parser's output flag
    const auto call = Codec::JSON::parse(calls[0].text, isJSON);
    REQUIRE(isJSON);
    CHECK(call.stringOr("method") == "host.sessions.subscribe");
    CHECK(factory.hasCanonicalTranscript());
    CHECK(host.plane().connection().answerCall(calls[0].callIndex,
              R"({"jsonrpc":"2.0","id":")" + call.stringOr("id")
                  + R"(","result":{"workspaces":[],"structureRevisions":[],"paneRevisions":[],"panes":0,"hostNowUnixMilliseconds":1}})")
        == Protocol::Sync::CallAnswer::Sent);
    observer.m_onDirectory = [&] { core.reset(); };
    pumpData(factory.last(), dispatcher, host.flush());
    CHECK(!core);
    CHECK(observer.m_directoryAnswers == std::vector<std::uint32_t> { requestID });
    CHECK(observer.m_directoryFailures.empty());
    dispatcher.fireDelayed();
    CHECK(observer.m_directoryAnswers.size() == 1);
}

TEST(directoryTimeoutEndsOnlyItsSubscriptionAndAllowsAnotherRequest)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    const auto first = core.requestSessionList();
    REQUIRE(first);
    host.receive(factory.sentLog());
    REQUIRE(host.plane().connection().takeCalls().size() == 1);
    dispatcher.fireDelayed();
    CHECK(observer.m_directoryFailures == std::vector<std::uint32_t> { first });
    CHECK(core.acknowledged());
    host.receive(factory.sentLog());
    const auto cleanup = host.plane().connection().takeCalls();
    REQUIRE(cleanup.size() == 1);
    bool isJSON = false; // bool-ok: the parser's output flag
    const auto ended = Codec::JSON::parse(cleanup[0].text, isJSON);
    CHECK(isJSON && ended.stringOr("method") == "host.sessions.unsubscribe");
    const auto second = core.requestSessionList();
    CHECK(second > first);
    core.stop();
    dispatcher.fireDelayed();
    CHECK(observer.m_directoryFailures.size() == 1);
}

TEST(test_bell_passthrough)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setClock([&] { return host.plane().now(); });
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    for (unsigned round = 0; round < 4; ++round) {
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.flush());
        dispatcher.fireDelayed();
    }
    pumpData(factory.last(), dispatcher, bellFrame(host, factory));
    check(observer.m_bells == 1, "typed bell forwarded to observer");
    term.reset();
}

// A passive remote client never parses raw bytes (no VT parser client-side),
// so it can't derive OSC 133;D's exit code itself — CommandFinished is the
// typed effect the host sends every client. Verify-by-revert: reverting
// RemoteSessionCore::onCommandDone to the base class's no-op default drops
// this straight to 0/empty and fails both checks.
TEST(test_command_done_passthrough)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setClock([&] { return host.plane().now(); });
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    for (unsigned round = 0; round < 4; ++round) {
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.flush());
        dispatcher.fireDelayed();
    }
    host.receive(factory.sentLog());
    const auto commandGeneration = host.plane().source().generation();
    CHECK(host.plane().source().appendEffect(Protocol::Sync::CommandFinishedEffect { .exitStatus = 2, .durationMicroseconds = 42, .currentDirectory = "/tmp/example" }, host.plane().connection().clientInstance()));
    pumpData(factory.last(), dispatcher, host.flush());
    check(observer.m_commandDones == 1, "typed command completion forwarded to observer");
    check(observer.m_lastExitCode == 2, "exit code forwarded intact");
    check(observer.m_lastCurrentDirectory == "/tmp/example", "current directory forwarded intact");
    CHECK_EQ(observer.m_lastCommandGeneration, commandGeneration);
    CHECK_EQ(observer.m_lastCommandSequence, 1u);
    CHECK_EQ(observer.m_lastCommandDurationMicroseconds, 42u);
    CHECK(observer.m_lastCommandDisposition == Protocol::Sync::EffectDisposition::Post);
    term.reset();
}

TEST(test_drop_reconnects)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(77));

    pumpClose(factory.last(), dispatcher, "ssh blip");
    check(core.state() == State::Reconnecting, "state Reconnecting on a transport drop");
    check(dispatcher.hasDelayed(), "a reconnect is scheduled (delayed)");
    check(factory.createCount() == 1, "no immediate reconnect (waits for backoff)");

    dispatcher.fireDelayed();
    check(factory.createCount() == 2, "reconnect creates a fresh channel after backoff");
    pumpConnect(factory.last(), dispatcher);
    check(factory.sentAnyOfType(helloType), "reconnect re-sends Greet");
    // The reattach Greet carries the pinned session id (77), not a fresh 0.
    pumpData(factory.last(), dispatcher, helloAckFrame(77));
    check(core.sessionID() == 77, "reattach keeps the session id");

    term.reset();
}

TEST(test_exit_does_not_reconnect)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(9));

    REQUIRE(observer.m_failureReasons.empty());
    // A real carrier can enqueue EOF behind the normal Bye. Queue both
    // before pumping: decoding the Bye retires that exact channel.
    factory.last()->fireData(exitFrame());
    queueClose(factory.last(), "post-exit drop");
    dispatcher.pump();
    check(observer.m_exits == 1, "Normal Bye forwarded to observer");
    check(core.state() == State::Failed, "state Failed after Normal Bye (no reconnect)");
    REQUIRE(observer.m_failureReasons.size() == 1);
    CHECK_STR_EQ(observer.m_failureReasons.front(), "The session ended.");
    CHECK(!factory.last());
    // Screen ticks queued while the connection was live still exist. Fire
    // them: a terminal session must not let any stale timer open a carrier.
    dispatcher.fireDelayed();
    CHECK(core.state() == State::Failed);
    check(!dispatcher.hasDelayed(), "no reconnect scheduled after Normal Bye");
    check(factory.createCount() == 1, "Normal Bye path never reconnects");

    term.reset();
}

TEST(test_malformed_bye_is_not_a_normal_session_end)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(9));

    std::vector<std::uint8_t> malformed;
    CHECK(Protocol::Sync::appendFrame(malformed, Protocol::Sync::FrameKind::Bye, 0, {}));
    pumpData(factory.last(), dispatcher, malformed);
    CHECK_EQ(observer.m_exits, 0);
    CHECK(core.state() == State::Reconnecting);
    CHECK(observer.m_failureReasons.empty());
    const auto events = core.trace().events(1);
    REQUIRE(events.size() == 1);
    CHECK_STR_EQ(events.front().text, "Reconnecting — The connection ended on data that couldn’t be read.");
}

TEST(test_stop_sends_exit_and_is_terminal)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));

    core.stop();
    check(factory.sentAnyOfType(exitType), "stop() sends an Normal Bye frame");
    check(core.state() == State::Failed, "stop() is terminal (Failed)");

    // stop() tore the transport down. Previously queued screen ticks may
    // remain, but firing them must neither schedule nor create a reconnect.
    dispatcher.fireDelayed();
    CHECK(core.state() == State::Failed);
    CHECK(!factory.last());
    check(!dispatcher.hasDelayed(), "stop() schedules no reconnect");
    check(factory.createCount() == 1, "no reconnect after stop()");
    term.reset();
}

TEST(test_quic_preack_drop_latches_fallback)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(true); // the first channel claims to be QUIC
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

    core.start(0, 80, 24);
    check(factory.lastPreferQUIC() == true, "first attempt prefers QUIC");
    check(core.isUsingQUIC(), "transport reports QUIC");
    // Drop before Welcome: QUIC never really connected → latch the fallback.
    pumpClose(factory.last(), dispatcher, "udp blocked");
    dispatcher.fireDelayed();
    check(factory.createCount() == 2, "reconnect after a pre-ack QUIC drop");
    check(factory.lastPreferQUIC() == false, "reconnect falls back off QUIC after a pre-ack drop");

    term.reset();
}

// The SSH bootstrap answered and the QUIC connection it pointed at closed
// before the host's HELLO_ACK. The session goes on over SSH, never to a
// failure that blames reaching the host, and the carrier fact names QUIC as
// what did not work.
TEST(aQUICHandshakeThatFailsAfterTheBootstrapFallsBackToSSHAndSaysQUIC)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(true);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    check(factory.lastPreferQUIC(), "the first dial asks for QUIC");
    pumpClose(factory.last(), dispatcher, "handshake timed out");
    check(core.state() != State::Failed, "a QUIC handshake failure is not the session's failure");
    factory.setOfferQUIC(false);
    dispatcher.fireDelayed();
    check(!factory.lastPreferQUIC(), "the redial is the SSH-only carrier");
    pumpConnect(factory.last(), dispatcher);
    const CarrierFact fact = core.carrierFact();
    check(fact.carrier == Carrier::Stream, "the session rides SSH");
    check(fact.quicFallbackReason.find("QUIC") != std::string::npos, "the fallback names QUIC");
    check(fact.quicFallbackReason.find("SSH") != std::string::npos, "the fallback names the SSH carrier it uses");
    check(fact.quicFallbackReason.find("reach") == std::string::npos, "the fallback never blames reaching the host");
    core.networkPathChanged();
    check(!factory.lastPreferQUIC(), "a path report keeps the session on SSH after QUIC failed");
    check(core.carrierFact().quicFallbackReason.find("QUIC") != std::string::npos, "the fallback remains visible");
    core.stop();
}

TEST(aPathChangeTellsAConnectedQUICCarrierToMove)
{
    for (const bool isQUIC : { true, false }) {
        WelcomedSession session(isQUIC ? TransportMode::QUIC : TransportMode::TCP);
        check(session.core->state() == State::Connected, "the session reached Connected");
        check(session.core->isUsingQUIC() == isQUIC, "the transport is the one offered");
        FakeChannel* channel = session.factory.last();
        check(channel != nullptr && channel->repaths() == 0, "the carrier was told to move before any path change");
        session.core->networkPathChanged();
        check(session.factory.last() == channel, "the path change replaced a connected carrier at once");
        if (session.factory.last() == channel)
            check(channel->repaths() == (isQUIC ? 1 : 0), isQUIC ? "a connected QUIC carrier was not told to move" : "a stream carrier was told to move");
    }
}

/// A path report cannot revive a QUIC carrier that failed before Welcome.
TEST(aNetworkChangeKeepsTheStreamFallbackAfterQUICFailed)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(true);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpClose(factory.last(), dispatcher, "udp blocked");
    factory.setOfferQUIC(false);
    dispatcher.fireDelayed();
    check(!factory.lastPreferQUIC(), "a QUIC drop before its handshake falls back");

    core.networkPathChanged();
    check(!factory.lastPreferQUIC(), "the redial on a new path still asks for SSH");
}

/// A wake cannot revive a QUIC carrier that failed before Welcome.
TEST(aResumeKeepsTheStreamFallbackAfterQUICFailed)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(true);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpClose(factory.last(), dispatcher, "udp blocked");
    factory.setOfferQUIC(false);
    dispatcher.fireDelayed();
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(core.state() == State::Connected && !core.isUsingQUIC(), "the session runs on the stream carrier");

    core.resumeAfterSuspension(5.0);
    // The stream connection is asked to answer first; one that stays silent
    // is replaced with another stream carrier.
    dispatcher.fireDelayed();
    check(!factory.lastPreferQUIC(), "the redial after a resume still asks for SSH");
}

/// A person who asks to reconnect is not made to wait out the backoff.
///
/// The backoff grows to 30 s while the person watches; a host they know is
/// back is dialled when they ask, and the timer that was waiting opens no
/// second channel beside it.
TEST(reconnectingNowSkipsTheBackoffWithoutASecondChannel)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpClose(factory.last(), dispatcher, "drop");
    check(core.state() == State::Reconnecting, "the drop waits on a backoff");
    const int createsBefore = factory.createCount();

    check(core.reconnectNow(), "a reconnecting session redials when asked");
    check(factory.createCount() == createsBefore + 1, "the redial opened a channel at once");
    check(core.state() == State::Connecting, "and waits for its handshake");

    dispatcher.fireDelayed();
    check(factory.createCount() == createsBefore + 1, "the abandoned backoff timer opened no second channel");

    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(!core.reconnectNow(), "a connected session is not torn down by the ask");
    check(factory.createCount() == createsBefore + 1, "and opens nothing");
}

TEST(test_reclaim_handoff)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(33, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 33));
    CHECK(!core.requestReclaim());
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    const Protocol::Control::ReclaimTarget target { .handle = host.plane().handle(), .sessionID = 33, .daemonInstanceID = 17, .hostInstanceID = { 17, 0 }, .attachmentID = { 1, 17 }, .paneID = { 33, 17 } };

    pumpData(factory.last(), dispatcher, reclaimReleasedFrame(target, "reclaim:1"));
    CHECK_EQ(observer.m_reclaims, 0);
    REQUIRE(core.requestReclaim());
    CHECK(!core.requestReclaim());
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false;
    const auto request = Codec::JSON::parse(calls.front().text, isJSON);
    REQUIRE(isJSON && request.find("params"));
    CHECK(request.stringOr("method") == "pane.reclaim");
    const auto intent = Protocol::Control::readReclaimRequest(*request.find("params"));
    REQUIRE(intent);
    CHECK_EQ(intent->handle, target.handle);
    CHECK_EQ(intent->daemonInstanceID, 17u);
    CHECK_EQ(request.find("params")->members().size(), 2u);
    pumpData(factory.last(), dispatcher, reclaimReleasedFrame(target, request.stringOr("id")));
    CHECK_EQ(observer.m_reclaims, 1);
    pumpData(factory.last(), dispatcher, reclaimReleasedFrame(target, request.stringOr("id")));
    CHECK_EQ(observer.m_reclaims, 1);

    pumpClose(factory.last(), dispatcher, "reclaim drop");
    CHECK_EQ(factory.createCount(), 1);
}

TEST(test_reclaim_close_before_ack_reconnects)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(33, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 33));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    REQUIRE(core.requestReclaim());
    host.receive(factory.sentLog());
    REQUIRE(host.plane().connection().takeCalls().size() == 1);
    pumpClose(factory.last(), dispatcher, "drop before reclaim Result");
    CHECK_EQ(observer.m_reclaims, 0);
    CHECK(core.state() == State::Reconnecting);
    dispatcher.fireDelayed();
    CHECK_EQ(factory.createCount(), 2);
}

TEST(aReclaimResultCannotReleaseAnotherPaneAttachmentOrServingIncarnation)
{
    for (int changed = 0; changed < 7; ++changed) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        UbiquittyTest::HostScreen host(33, 17);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 33));
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.publish());
        REQUIRE(core.requestReclaim());
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false;
        const auto request = Codec::JSON::parse(calls.front().text, isJSON);
        REQUIRE(isJSON);
        Protocol::Control::ReclaimTarget wrong { .handle = host.plane().handle(), .sessionID = 33, .daemonInstanceID = 17, .hostInstanceID = { 17, 0 }, .attachmentID = { 1, 17 }, .paneID = { 33, 17 } };
        switch (changed) {
        case 0:
            ++wrong.handle;
            break;
        case 1:
            ++wrong.sessionID;
            break;
        case 2:
            ++wrong.daemonInstanceID;
            break;
        case 3:
            ++wrong.hostInstanceID.low;
            break;
        case 4:
            ++wrong.attachmentID.low;
            break;
        case 5:
            ++wrong.paneID.low;
            break;
        case 6:
            break;
        }
        const auto id = request.stringOr("id") + (changed == 6 ? ":unrelated" : "");
        pumpData(factory.last(), dispatcher, reclaimReleasedFrame(wrong, id));
        CHECK_EQ(observer.m_reclaims, 0);
        pumpClose(factory.last(), dispatcher, "not a successful reclaim");
        CHECK(core.state() == State::Reconnecting);
    }
}

// A fresh host: the bootstrap prints the deploy sentinel (with the remote's
// uname) before any handshake, then ssh exits. The core must surface a deploy
// request, not loop reconnecting.
TEST(test_deploy_sentinel_pre_ack)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    FakeChannel* first = factory.last();

    // Model the real race: ssh prints the sentinel and then exits, so the I/O
    // thread posts onData(sentinel) and onClose(ssh exit) back to back, before
    // the session thread runs either. The sentinel handler runs first and tears
    // the channel down (teardownChannel deletes it and bumps the generation), so
    // the already-queued close — now stale-generation — is dropped on the same
    // pump without touching the destroyed channel. (Queue the close FIRST, while
    // `first` is still alive: firing it after the sentinel pump would dereference
    // a freed FakeChannel.)
    first->fireData(sentinelBytes("login shell startup output\nUBIQUITTY-NEED-DEPLOY Linux x86_64\n"));
    queueClose(first, "ssh exited after sentinel");
    dispatcher.pump();

    check(core.state() == State::Deploying, "sentinel moves the core to Deploying");
    check(observer.m_deploys == 1, "onDeployRequested fired once");
    check(observer.m_deployPlatform == "Linux x86_64", "platformArch parsed from the sentinel");
    check(!dispatcher.hasDelayed(), "no reconnect scheduled while deploying");
    check(factory.createCount() == 1, "no fresh channel during deploy");
    // The ssh exit that followed the sentinel was ignored (generation guard).
    check(core.state() == State::Deploying, "post-sentinel ssh close is dropped");
    check(!dispatcher.hasDelayed(), "post-sentinel close schedules no reconnect");

    term.reset();
}

// The sentinel can arrive split across reads (ssh chunks the pipe); the sniffer
// must buffer a partial line.
TEST(test_deploy_sentinel_fragmented)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    pumpData(factory.last(), dispatcher, sentinelBytes("UBIQUITTY-NE"));
    check(core.state() == State::Connecting, "partial sentinel keeps waiting");
    check(observer.m_deploys == 0, "no deploy on a partial sentinel");
    pumpData(factory.last(), dispatcher, sentinelBytes("ED-DEPLOY Darwin arm64\n"));
    check(core.state() == State::Deploying, "completed sentinel triggers deploy");
    check(observer.m_deployPlatform == "Darwin arm64", "platformArch parsed across fragments");

    term.reset();
}

// A failed deploy is terminal — no silent reconnect loop.
TEST(test_deploy_failure_is_terminal)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, sentinelBytes("UBIQUITTY-NEED-DEPLOY Linux x86_64\n"));

    core.deployFinished(DeployResult::Failed, "build failed");
    check(core.state() == State::Failed, "a failed deploy is terminal");
    check(!dispatcher.hasDelayed(), "no reconnect after a failed deploy");
    check(factory.createCount() == 1, "failed deploy never reconnects");

    term.reset();
}

// A successful deploy reconnects and completes the handshake against the now
// freshly-installed binary.
TEST(test_deploy_success_reconnects)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, sentinelBytes("UBIQUITTY-NEED-DEPLOY Linux x86_64\n"));

    core.deployFinished(DeployResult::Succeeded, "");
    check(factory.createCount() == 2, "a successful deploy creates a fresh channel");
    pumpConnect(factory.last(), dispatcher);
    check(factory.sentAnyOfType(helloType), "the reconnect re-sends Greet");
    pumpData(factory.last(), dispatcher, helloAckFrame(88));
    check(core.state() == State::Connected, "the session connects after deploy");
    check(core.sessionID() == 88, "sessionID pinned post-deploy");

    term.reset();
}

// A second sentinel after a successful deploy means the pushed binary is broken
// — fail rather than re-deploy forever.
TEST(test_second_sentinel_fails)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, sentinelBytes("UBIQUITTY-NEED-DEPLOY Linux x86_64\n"));
    core.deployFinished(DeployResult::Succeeded, "");
    pumpConnect(factory.last(), dispatcher);

    pumpData(factory.last(), dispatcher, sentinelBytes("UBIQUITTY-NEED-DEPLOY Linux x86_64\n"));
    check(core.state() == State::Failed, "a second sentinel is terminal");
    check(observer.m_deploys == 1, "no second deploy request");
    check(!dispatcher.hasDelayed(), "no reconnect after the second sentinel");

    term.reset();
}

// The sniff must not disturb a normal handshake: a Welcome as the very first
// inbound bytes (kind Welcome) resolve the sniffer on byte 0 and decode.
TEST(test_non_sentinel_preack_data_still_decodes)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(core.state() == State::Connected, "Welcome as first bytes still connects");
    check(core.sessionID() == 7, "sessionID pinned through the sniff path");
    check(observer.m_deploys == 0, "no spurious deploy on protocol traffic");

    term.reset();
}

// Read a passive Ubiquitty::Terminal row as printable ASCII (for a coarse content check).
std::string rowText(Ubiquitty::Terminal& t, int row)
{
    std::string s;
    for (int c = 0; c < t.columns; ++c) {
        const Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, c);
        const std::uint32_t cp = cell ? cell->codePoint : 0;
        s.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : ' ');
    }
    return s;
}

// An image arrives on its own frame with no cell change behind it. A client that
// repaints on grid movement alone holds the frame it has until something
// unrelated moves, so the picture appears late and at random — the shape a
// count-only assertion on frames received cannot distinguish from working.
TEST(test_an_image_frame_repaints)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 12, 4);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen sessionHost(7, 17, 4, 12);
    pumpData(factory.last(), dispatcher, welcomeOnly(sessionHost, factory, 7));

    auto connectionBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(Protocol::Sync::maxCatalogMessageBytes);
    auto aggregateBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(Protocol::Sync::maxCatalogMessageBytes);
    sessionHost.plane().connection().setImageSourceAdmission([&] {
        const auto retained = sessionHost.plane().connection().retainedOutboundPayloadBytes();
        return Protocol::Sync::BulkSourceAdmission { .connection = connectionBudget, .aggregate = aggregateBudget, .otherConnectionBytes = retained, .otherAggregateBytes = retained };
    });
    Ubiquitty::Terminal* host = &sessionHost.terminal();
    sessionHost.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, sessionHost.publish());
    const int afterCheckpoint = observer.m_screens;
    check(afterCheckpoint == 1, "the checkpoint fired one repaint");

    // Store a 2x2 image on the HOST and ship it the way the daemon does. The
    // pixels are the store's to own, so they are handed over as a fresh buffer.
    auto* pixels = static_cast<unsigned char*>(std::malloc(2 * 2 * 4));
    check(pixels != nullptr, "the fixture allocated its pixels");
    std::memset(pixels, 0xFF, 2 * 2 * 4);
    check(Ubiquitty::Image::storeRGBAImage(host, pixels, 2, 2, 1, 1), "the host stored the image");

    const auto imageCount = [&] { return std::ranges::count_if(term->images.stored, [](const auto& image) { return image.used; }); };
    const auto placementCount = [&] { return std::ranges::count_if(term->images.placements, [](const auto& placement) { return placement.used; }); };
    CHECK(imageCount() == 0);
    CHECK(placementCount() == 0);
    // The typed host offers the immutable image, reads ImageWant, and sends
    // bounded payload chunks. No retired outer image frame is injected.
    for (unsigned pass = 0; pass < 8 && !placementCount(); ++pass) {
        REQUIRE(factory.last());
        sessionHost.receive(factory.sentLog());
        sessionHost.plane().advanceClock(16'667);
        pumpData(factory.last(), dispatcher, sessionHost.publish());
    }
    REQUIRE(imageCount() == 1);
    CHECK(term->images.stored[0].width == 2 && term->images.stored[0].height == 2);
    CHECK(std::memcmp(term->images.stored[0].pixels, host->images.stored[0].pixels, 16) == 0);
    CHECK(placementCount() == 1);
    CHECK(rowText(*term, 0) == rowText(*host, 0));

    check(observer.m_screens > afterCheckpoint,
        "an image with no cell change behind it still reaches the screen");
}

// An update applies onto the passive Terminal and fires the same
// onScreenChanged the frontend repaints on. The pixels are the user's visual
// check; this guards the wiring.
TEST(test_an_update_applies_and_repaints)
{
    auto term = makeTerm(); // the client's passive render target
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 12, 4);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(7, 17, 4, 12);
    pumpData(factory.last(), dispatcher, host.welcome(factory));

    Ubiquitty::processBytes(&host.terminal(), "alpha line\r\nbravo line\r\n");
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(observer.m_screens == 1, "the checkpoint fires one repaint");
    check(rowText(*term, 0).starts_with("alpha"), "client shows the checkpoint's row 0");

    Ubiquitty::processBytes(&host.terminal(), std::string_view("\x1b[2;1HBRAVO EDITED"));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(observer.m_screens == 2, "the update fires the repaint trigger too");
    check(rowText(*term, 1).starts_with("BRAVO"), "client Terminal reflects the update");
    check(rowText(*term, 0).starts_with("alpha"), "the unchanged row 0 is untouched");

    term.reset();
}

// The less -X remote staircase regression. A primary-screen pager fills the grid;
// the host resizes it geometry-only (foregroundIsShell=false → no reflow) so the
// host scrollback stays clean. The thin client must make the same decision —
// mirror PaneFacts.isAtPrompt and gate its OWN reflow — or its reflow drives
// its live rows into its own LineStore on every drag tick (a staircase the host
// never had and never re-streams away).
TEST(test_pager_resize_does_not_manufacture_scrollback)
{
    SessionHost host(9, 17, 6, 20);
    const char* page = "row-one\r\nrow-two\r\nrow-three\r\nrow-four\r\nrow-five\r\nrow-six";
    Ubiquitty::processBytes(&host.terminal(), page);
    check(Ubiquitty::Screen::scrollbackCount(&host.terminal()) == 0, "host pager page: clean scrollback");

    auto term = makeTerm(); // the client's passive mirror
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 20, 6);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, host.welcome(factory));

    check(core.foregroundIsShell(), "foregroundIsShell defaults to shell before any PaneFacts");

    // Mirror the host's page, then learn the pager owns the foreground (less -X).
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(Ubiquitty::Screen::scrollbackCount(term.get()) == 0, "client mirror starts clean");
    pumpData(factory.last(), dispatcher, statusFrame(/*foregroundIsShell=*/false, "less"));
    check(!core.foregroundIsShell(), "PaneFacts.isAtPrompt=false gates pager reflow");

    // The window controller's remote-resize gate: pager
    // foreground → reflow suppressed → geometry-only → no manufacture.
    const bool allowReflow = Ubiquitty::shouldReflowOnResize(term.get(), core.foregroundIsShell());
    check(!allowReflow, "pager foreground → client resize gated to geometry-only");
    Ubiquitty::resize(*term, 4, 14,
        allowReflow ? Ubiquitty::Modes::ReflowMode::Reflow : Ubiquitty::Modes::ReflowMode::Fixed); // window drag: rows + cols shrink
    check(Ubiquitty::Screen::scrollbackCount(term.get()) == 0, "gated geometry-only resize manufactures NO scrollback (no staircase)");

    // Control: an unconditional reflow on the same pager screen does push rows
    // into scrollback — i.e. the gate is exactly what prevents the staircase.
    auto bug = makeTerm();
    Ubiquitty::resize(*bug, 6, 20);
    Ubiquitty::processBytes(bug.get(), page);
    Ubiquitty::resize(*bug, 4, 14, Ubiquitty::Modes::ReflowMode::Reflow);
    check(Ubiquitty::Screen::scrollbackCount(bug.get()) > 0, "control: unconditional reflow on a pager screen DOES manufacture scrollback");
    bug.reset();

    // Back at the shell prompt: reflow re-enabled (the mirror re-wraps locally,
    // matching the host, with no re-download — the point of the client mirror).
    pumpData(factory.last(), dispatcher, statusFrame(/*foregroundIsShell=*/true, "-zsh"));
    check(core.foregroundIsShell(), "PaneFacts.isAtPrompt=true permits shell reflow");
    check(Ubiquitty::shouldReflowOnResize(term.get(), core.foregroundIsShell()), "shell foreground → client reflows mirrored scrollback on resize");

    term.reset();
}

// The remote less -X resize bug: an update that cannot apply — the client
// resized its mirror geometry-only ahead of the host — must make the client ask
// for a checkpoint so the host re-baselines. A refusal that only traced would
// leave the screen stale until the host emitted more output. The client stays
// geometry-only (no local reflow), so the staircase fix is preserved.
TEST(test_an_unappliable_update_asks_for_a_checkpoint)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(5, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));

    Ubiquitty::processBytes(&host.terminal(), "baseline row\r\n");
    pumpData(factory.last(), dispatcher, host.publish(factory));
    const auto resync = Protocol::Sync::FrameKind::Resync;
    check(factory.sentScreenFramesOfKind(resync) == 0, "no checkpoint is asked for after a clean one");

    // The window shrinks: the client resizes its mirror geometry-only to 80x20,
    // ahead of the host. An update still at 80x24 (less's pre-resize repaint)
    // now has mismatched dims and cannot apply.
    Ubiquitty::resize(*term, 20, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::processBytes(&host.terminal(), "\x1b[1;1Hchanged text");
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(factory.sentScreenFramesOfKind(resync) == 1, "an unappliable update asks the host for a checkpoint");

    // The checkpoint that answers it brings the mirror to the host's grid.
    pumpData(factory.last(), dispatcher, host.publish(factory));
    check(term->rows == 24 && rowText(*term, 0).starts_with("changed text"), "and the checkpoint re-baselines the mirror");

    term.reset();
}

} // namespace

// ---- the client's outbound commands ----------------------------------
// Revoking a consent grant, answering a prompt, and asking for a file all use
// correlated Calls only after the acknowledged session has a bound screen.
TEST(test_outbound_commands_are_gated_on_the_ack)
{
    namespace Control = Protocol::Control;
    namespace Sync = Protocol::Sync;
    const std::string attachmentID = "00000000000000000000000000000007";
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

    // Before start there is no channel: every one of them is a no-op rather
    // than a write through a null.
    core.revokeConsent(0, attachmentID, {});
    core.respondToConsent(11, Protocol::Sync::ConsentKind::Attach, 1);
    CHECK(core.requestFile("/etc/hosts") == 0);
    check(factory.sentCount() == 0, "nothing is sent before start");

    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);

    // Connected but not acked: still gated.
    core.revokeConsent(0, attachmentID, {});
    core.respondToConsent(11, Protocol::Sync::ConsentKind::Attach, 1);
    check(core.requestFile("/etc/hosts") == 0, "a file request before the ack has no id");
    const std::size_t beforeAck = factory.sentCount();

    UbiquittyTest::HostScreen fileHost(4242, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(fileHost, factory, 4242));

    fileHost.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, fileHost.publish());

    // Each operation uses its admitted transport after Welcome and pane binding.
    core.revokeConsent(0, attachmentID, {});
    core.respondToConsent(11, Protocol::Sync::ConsentKind::Attach, 1);
    const uint32_t requestID = core.requestFile("/etc/hosts");
    check(requestID != 0, "a file request past the ack gets an id");
    fileHost.receive(factory.sentLog());
    const auto calls = fileHost.plane().connection().takeCalls();
    REQUIRE(calls.size() == 3);
    const auto revoke = Control::decodeRequest(calls[0].text, {});
    REQUIRE(revoke.outcome == Control::RequestOutcome::Dispatch);
    CHECK(revoke.method == "consent.revoke");
    REQUIRE(revoke.hasID && revoke.id.isString() && !revoke.id.asString().empty());
    const auto revocation = Control::readConsentRevocation(revoke.params);
    REQUIRE(revocation);
    CHECK(revocation->sessionID == 4242);
    CHECK(revocation->action == Sync::ConsentRevocationKind::DisconnectNow);
    CHECK(revocation->attachmentID == (Sync::ID128 { .low = 7, .high = 0 }));
    CHECK(revocation->identity.empty());

    const auto respond = Control::decodeRequest(calls[1].text, {});
    REQUIRE(respond.outcome == Control::RequestOutcome::Dispatch);
    CHECK(respond.method == "consent.respond");
    REQUIRE(respond.hasID && respond.id.isString() && !respond.id.asString().empty());
    CHECK(respond.id.asString() != revoke.id.asString());
    const auto response = Control::readConsentResponse(respond.params);
    REQUIRE(response);
    CHECK(response->requestID == 11);
    CHECK(response->kind == Sync::ConsentKind::Attach);
    CHECK(response->decision == Sync::ConsentChoice::AllowOnce);

    const auto file = Control::decodeRequest(calls[2].text, {});
    REQUIRE(file.outcome == Control::RequestOutcome::Dispatch);
    CHECK(file.method == "file.read");
    REQUIRE(fileHost.plane().handle() != 0);
    REQUIRE(fileHost.plane().handle() != 4242);
    CHECK(file.params.numberOr("pane") == fileHost.plane().handle());
    CHECK(file.params.stringOr("path") == "/etc/hosts");
    CHECK(factory.hasCanonicalTranscript());
    CHECK(factory.sentCalls("consent.revoke") == 1);
    CHECK(factory.sentCalls("consent.respond") == 1);
    CHECK(factory.sentCalls("file.read") == 1);
    check(factory.sentCount() > beforeAck, "the gated calls sent nothing before the ack");

    // Request ids are handed out in order, so two requests are answerable
    // apart — the reply carries the id and nothing else identifies it.
    check(core.requestFile("/etc/passwd") == requestID + 1, "request ids advance");
}

TEST(aNativePasteWaitsForCreditAndRetiresItsOwnerBeforeCallbackDestruction)
{
    using namespace Protocol::Control;
    namespace Sync = Protocol::Sync;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    auto core = std::make_unique<RemoteSessionCore>(term.get(), &dispatcher, &factory, &observer);
    const std::array<std::uint8_t, 8> payload { 0x89, 'P', 'N', 'G', 0, 255, 128, 0 };
    CHECK_EQ(core->pushPaste("image/png", payload), 0u);
    core->start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    CHECK_EQ(core->pushPaste("image/png", payload), 0u);
    UbiquittyTest::HostScreen host(4242, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 4242));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    factory.last()->setPendingOutboundBytes(0);
    const auto pushID = core->pushPaste("image/png", payload);
    REQUIRE(pushID);
    CHECK(core->pastePushProgress() == 0);
    CHECK_EQ(core->pushPaste("image/png", payload), 0u);
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto call = Sync::decodeCallRequest(calls.front().text);
    REQUIRE(call.outcome == RequestOutcome::Dispatch);
    CHECK(call.method == "paste.push");
    REQUIRE(call.id.isString());
    const auto manifest = pastePushParameters(call.params);
    REQUIRE(manifest);
    CHECK(factory.hasCanonicalTranscript());

    // The only host receiver is this bounded memory owner, not Daemon or a
    // landing adapter. Queue admission/correlation is the oracle here.
    PasteUpload receiver;
    REQUIRE(receiver.begin(*manifest, 9, [](std::size_t) { return true; }));
    unsigned chunks = 0;
    host.plane().connection().setControlUploadReceiver([&](const Sync::BulkChunk& chunk) {
        ++chunks;
        CHECK_EQ(chunk.index, 0u);
        return receiver.accept(chunk, [] { return true; }) == PasteUploadProgress::Complete
            ? Sync::ClientBulkDisposition::Accepted : Sync::ClientBulkDisposition::Refused; }, [&](std::uint32_t transfer) { return receiver.transfer() == transfer; });
    REQUIRE(host.plane().connection().sendNotify(*pasteReadyNotify({ "another-call", manifest->pane, 9 })));
    pumpData(factory.last(), dispatcher, host.flush());
    host.receive(factory.sentLog());
    CHECK_EQ(chunks, 0u);

    auto* channel = factory.last();
    REQUIRE(channel);
    channel->setOutboundCapacityBytes(Transport::guaranteedOutboundFrameBytes);
    channel->setPendingOutboundBytes(Transport::guaranteedOutboundFrameBytes);
    const auto credit = pasteReadyNotify({ call.id.asString(), manifest->pane, 9 });
    REQUIRE(credit);
    REQUIRE(host.plane().connection().sendNotify(*credit));
    REQUIRE(receiver.grantCredit(9));
    pumpData(factory.last(), dispatcher, host.flush());
    host.receive(factory.sentLog());
    CHECK_EQ(chunks, 0u);
    CHECK(observer.m_pastes.empty());
    channel->setPendingOutboundBytes(0);
    REQUIRE(host.plane().connection().sendNotify(*credit));
    pumpData(factory.last(), dispatcher, host.flush());
    host.receive(factory.sentLog());
    CHECK_EQ(chunks, 1u);
    CHECK(std::ranges::equal(receiver.data(), payload));
    CHECK(core->pastePushProgress() == 1);
    channel->setPendingOutboundBytes(payload.size() / 2);
    CHECK(core->pastePushProgress() == 0.5);
    channel->setPendingOutboundBytes(-1);
    CHECK(core->pastePushProgress() < 0);
    channel->setPendingOutboundBytes(0);

    auto result = Codec::JSON::Value::object();
    result.set("pane", Codec::JSON::Value::number(manifest->pane));
    result.set("status", Codec::JSON::Value::string("busy"));
    result.set("path", Codec::JSON::Value::string("/tmp/not-a-real-file.png"));
    const auto response = Codec::JSON::serialize(rpcResult(call.idOrNull(), std::move(result)));
    REQUIRE(host.plane().connection().answerCall(calls.front().callIndex, response) == Sync::CallAnswer::Sent);
    observer.m_onPaste = [&] { core.reset(); };
    pumpData(factory.last(), dispatcher, host.flush());
    CHECK(!core);
    REQUIRE(observer.m_pastes.size() == 1);
    CHECK_EQ(observer.m_pastes.front().pushID, pushID);
    CHECK(observer.m_pastes.front().status == PastePushStatus::Busy);
    CHECK(observer.m_pastes.front().path == "/tmp/not-a-real-file.png");
}

TEST(fileDownloadApprovalSendsOnlyTheCapturedPathOnce)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    using Decision = FileDownloadDecision;
    CHECK(!core.prepareFileDownload(1, "build.example", "/srv/report.txt"));
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    CHECK(!core.prepareFileDownload(1, "build.example", "/srv/report.txt"));
    UbiquittyTest::HostScreen fileHost(4242, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(fileHost, factory, 4242));
    fileHost.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, fileHost.publish());

    CHECK(core.prepareFileDownload(1, "build.example", "/srv/cancelled.txt"));
    CHECK(factory.sentCalls("file.read") == 0);
    CHECK(core.answerFileDownload(1, Decision::Cancel) == 0);
    CHECK(core.answerFileDownload(1, Decision::Download) == 0);
    CHECK(factory.sentCalls("file.read") == 0);

    std::string path = "/srv/report.txt";
    std::string host = "build.example";
    CHECK(core.prepareFileDownload(2, host, path));
    path = "/srv/different.txt";
    host = "other.example";
    REQUIRE(core.pendingFileDownload());
    CHECK(core.pendingFileDownload()->host == "build.example");
    CHECK(core.pendingFileDownload()->path == "/srv/report.txt");
    CHECK(core.pendingFileDownload()->sessionID == 4242);
    CHECK(core.answerFileDownload(1, Decision::Download) == 0);
    CHECK(factory.sentCalls("file.read") == 0);
    const auto requestID = core.answerFileDownload(2, Decision::Download);
    CHECK(requestID != 0);
    CHECK(!core.pendingFileDownload());
    CHECK(factory.sentCalls("file.read") == 1);
    CHECK(core.answerFileDownload(2, Decision::Download) == 0);
    CHECK(factory.sentCalls("file.read") == 1);
    CHECK(factory.hasCanonicalTranscript());
    fileHost.receive(factory.sentLog());
    const auto calls = fileHost.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool valid = false; // bool-ok: parser output
    const auto request = Codec::JSON::parse(calls.front().text, valid);
    REQUIRE(valid && request.find("params"));
    CHECK(request.stringOr("method") == "file.read");
    CHECK(request.find("params")->stringOr("path") == "/srv/report.txt");

    // A line link's question carries the line, which the copy opens at once
    // it lands; what the host is asked for is the path alone.
    CHECK(core.prepareFileDownload(3, "build.example", "/srv/main.swift", FileLinkLine { 12, 5 }));
    REQUIRE(core.pendingFileDownload());
    CHECK(core.pendingFileDownload()->path == "/srv/main.swift");
    CHECK(core.pendingFileDownload()->line.line == 12 && core.pendingFileDownload()->line.column == 5);
    CHECK(core.answerFileDownload(3, Decision::Cancel) == 0);
    CHECK(core.prepareFileDownload(4, "build.example", "/srv/report.txt"));
    REQUIRE(core.pendingFileDownload());
    CHECK_MSG(core.pendingFileDownload()->line.line == 0, "a link that names no line keeps none");
    CHECK(core.answerFileDownload(4, Decision::Cancel) == 0);
}

TEST(fileDownloadApprovalDoesNotSurviveReconnectOrDetach)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    using Decision = FileDownloadDecision;
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(4242));
    CHECK(core.prepareFileDownload(1, "build.example", "/srv/report.txt"));
    pumpClose(factory.last(), dispatcher, "test disconnect");
    CHECK(!core.pendingFileDownload());
    CHECK(core.answerFileDownload(1, Decision::Download) == 0);
    dispatcher.fireDelayed();
    REQUIRE(factory.last());
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(4242));
    CHECK(core.acknowledged());
    CHECK(core.answerFileDownload(1, Decision::Download) == 0);
    CHECK(factory.sentCalls("file.read") == 0);
    CHECK(core.prepareFileDownload(2, "build.example", "/srv/report.txt"));
    core.stop();
    CHECK(!core.pendingFileDownload());
    CHECK(core.answerFileDownload(2, Decision::Download) == 0);
    CHECK(factory.sentCalls("file.read") == 0);
    CHECK(factory.hasCanonicalTranscript());
}

TEST(windowCloseRequiresFreshStructureAndReportsARefusedDirectSend)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    using Decision = RemoteWindowCloseDecision;
    const UbiquittyTest::StructurePane pane { 1, 7 };
    const auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", { &pane, 1 }, 17);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen firstHost(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(firstHost, factory, 7, 17, 91));
    firstHost.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(firstHost.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, firstHost.flush());
    REQUIRE(core.prepareWindowClose(1));
    CHECK(core.pendingWindowCloseSessionCount() == 1);
    pumpClose(factory.last(), dispatcher, "replace the direct carrier");
    CHECK(core.pendingWindowCloseToken() == 0);
    UbiquittyTest::HostScreen nextHost(7, 17);
    nextHost.passOver(factory.sentLog().size());
    dispatcher.fireDelayed();
    REQUIRE(factory.last());
    pumpConnect(factory.last(), dispatcher);
    const auto welcome = welcomeOnly(nextHost, factory, 7, 17, 91);
    REQUIRE(welcome.size() > 1);
    pumpData(factory.last(), dispatcher, { welcome.begin(), welcome.end() - 1 });
    CHECK(!core.acknowledged());
    pumpData(factory.last(), dispatcher, { welcome.end() - 1, welcome.end() });
    nextHost.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, nextHost.flush());
    REQUIRE(core.acknowledged());
    REQUIRE(core.workspace()); // retained for display, not fresh authority
    CHECK(!core.prepareWindowClose(2));
    CHECK(core.pendingWindowCloseToken() == 0);
    CHECK(core.answerWindowClose(1, Decision::CloseWindow) == 0);
    CHECK(factory.sentCalls("window.close") == 0);
    REQUIRE(UbiquittyTest::offerStructure(nextHost.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, nextHost.flush());
    REQUIRE(core.prepareWindowClose(3));
    CHECK(core.pendingWindowCloseToken() == 3);
    // This does not test output capacity: the direct channel's
    // send API is void. A missing carrier must still return no request ID.
    RemoteSessionWindowCloseAccess::removeCarrierWithoutStateChange(core);
    CHECK(core.answerWindowClose(3, Decision::CloseWindow) == 0);
    CHECK(core.pendingWindowCloseToken() == 0);
    CHECK(core.answerWindowClose(3, Decision::CloseWindow) == 0);
    CHECK(factory.sentCalls("window.close") == 0);
}

TEST(directTreeCallsUseTypedAnswersAndBoundPendingOwnership)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const UbiquittyTest::StructurePane pane { 1, 7 };
    const auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", { &pane, 1 }, 17);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.surfaceID = 1;
    verb.tabID = 1;
    verb.paneID = 1;
    verb.orientation = std::to_underlying(Panes::Orientation::Vertical);
    verb.ratio = Panes::defaultRatio;
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::AddTab, Ubiquitty::Session::WorkspaceIntentKind::SplitFocusedPane,
             Ubiquitty::Session::WorkspaceIntentKind::CloseTab, Ubiquitty::Session::WorkspaceIntentKind::ClosePane }) {
        verb.verb = operation;
        const auto requestID = core.sendWorkspaceVerb(verb);
        REQUIRE(requestID);
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: the parser's output flag
        const auto request = Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON);
        CHECK(request.stringOr("id") == "ubiquitty.tree." + std::to_string(requestID));
        const std::string_view result = operation == Ubiquitty::Session::WorkspaceIntentKind::AddTab ? R"({"tab":"tab:9","pane":"pane:42","session":"session:77"})"
            : operation == Ubiquitty::Session::WorkspaceIntentKind::SplitFocusedPane                 ? R"({"pane":"pane:43","session":"session:78"})"
            : operation == Ubiquitty::Session::WorkspaceIntentKind::CloseTab                         ? R"({"tab":"tab:1","sessionsEnded":1})"
                                                                                                     : R"({"pane":"pane:1","sessionsEnded":1})";
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + std::string(result) + "}")
            == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.flush());
        REQUIRE(!observer.m_workspaceResults.empty());
        const auto& answer = observer.m_workspaceResults.back();
        CHECK(answer.requestID == requestID && answer.wasPerformed);
        CHECK(answer.paneID == (operation == Ubiquitty::Session::WorkspaceIntentKind::AddTab ? 42u : operation == Ubiquitty::Session::WorkspaceIntentKind::SplitFocusedPane ? 43u
                                                                                                                                                                            : 0u));
    }
    CHECK(observer.m_workspaceResults.size() == 4);
    CHECK(factory.hasCanonicalTranscript());
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddTab;
    verb.paneID = 999;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.paneID = 1;
    for (std::size_t pending = 0; pending < Protocol::Sync::maxPendingStructureConsents; ++pending)
        REQUIRE(core.sendWorkspaceVerb(verb));
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    RemoteSessionWindowCloseAccess::removeCarrierWithoutStateChange(core);
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    CHECK(observer.m_workspaceResults.size() == 4);
}

TEST(directNamesAndLabelsRequireTheirExactTypedAnswer)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const UbiquittyTest::StructurePane pane { 1, 7 };
    const auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", { &pane, 1 }, 17);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.surfaceID = 1;
    verb.tabID = 1;
    verb.paneID = 1;
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace, Ubiquitty::Session::WorkspaceIntentKind::RenameTab,
             Ubiquitty::Session::WorkspaceIntentKind::LabelSurface, Ubiquitty::Session::WorkspaceIntentKind::LabelPane, Ubiquitty::Session::WorkspaceIntentKind::LabelSession, Ubiquitty::Session::WorkspaceIntentKind::LabelClient }) {
        verb.verb = operation;
        verb.paneID = operation == Ubiquitty::Session::WorkspaceIntentKind::LabelSurface ? 0 : 1;
        const bool isName = operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace || operation == Ubiquitty::Session::WorkspaceIntentKind::RenameTab;
        const bool isClient = operation == Ubiquitty::Session::WorkspaceIntentKind::LabelClient;
        const std::string target = operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace ? "workspace:91"
            : operation == Ubiquitty::Session::WorkspaceIntentKind::RenameTab                            ? "tab:1"
            : operation == Ubiquitty::Session::WorkspaceIntentKind::LabelSurface                         ? "window:1"
            : operation == Ubiquitty::Session::WorkspaceIntentKind::LabelPane                            ? "pane:1"
            : isClient                                                                                   ? "client:17"
                                                                                                         : "session:7";
        enum class AnswerShape {
            Exact,
            WrongText,
            WrongTarget,
            MissingText
        };
        for (const auto shape : { AnswerShape::WrongText, AnswerShape::WrongTarget, AnswerShape::MissingText, AnswerShape::Exact }) {
            verb.title = "Build — 日本語";
            const auto requestID = core.sendWorkspaceVerb(verb);
            REQUIRE(requestID);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            const auto& parameters = *request.find("params");
            const auto method = operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace ? "workspace.rename"
                : operation == Ubiquitty::Session::WorkspaceIntentKind::RenameTab                     ? "tab.rename"
                : isClient                                                                            ? "client.label"
                                                                                                      : "label.set";
            CHECK(request.stringOr("method") == method);
            CHECK(parameters.stringOr(isName ? "name" : "label") == verb.title);
            if (isName)
                CHECK(parameters.numberOr(operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace ? "workspace" : "tab") == (operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace ? 91 : 1));
            else if (isClient)
                CHECK(parameters.members().size() == 1 && !parameters.find("client") && !parameters.find("object"));
            else
                CHECK(parameters.stringOr("object") == target);
            auto result = Codec::JSON::Value::object();
            result.set(isName ? operation == Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace ? "workspace" : "tab" : isClient ? "client"
                                                                                                                                       : "object",
                Codec::JSON::Value::string(shape == AnswerShape::WrongTarget ? isClient ? "pane:17" : target + "0" : target));
            if (shape != AnswerShape::MissingText)
                result.set(isName ? "name" : "label", Codec::JSON::Value::string(shape == AnswerShape::WrongText ? "another operation" : verb.title));
            auto answer = Codec::JSON::Value::object();
            answer.set("jsonrpc", Codec::JSON::Value::string("2.0"));
            answer.set("id", *request.find("id"));
            answer.set("result", std::move(result));
            CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            CHECK(observer.m_workspaceResults.back().requestID == requestID);
            CHECK(observer.m_workspaceResults.back().wasPerformed == (shape == AnswerShape::Exact));
            CHECK(observer.m_workspaceResults.back().paneID == 0 && observer.m_workspaceResults.back().sessionID == 0);
        }
    }
    CHECK(factory.hasCanonicalTranscript());
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace;
    verb.title.assign(4097, 'n');
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.title.assign(4096, 'n');
    CHECK(core.sendWorkspaceVerb(verb) != 0);
    verb.title = std::string(1, static_cast<char>(0xFF));
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.title.clear();
    CHECK(core.sendWorkspaceVerb(verb) != 0);
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::LabelPane;
    verb.title.assign(65, 'l');
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.title.assign(64, 'l');
    CHECK(core.sendWorkspaceVerb(verb) != 0);
    verb.title = "pane:7";
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.title = "line\nfeed";
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.title.clear();
    CHECK(core.sendWorkspaceVerb(verb) != 0);
    verb.title = "valid";
    verb.paneID = 999;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::LabelClient;
    for (const auto& invalid : { std::string(65, 'l'), std::string("client:17"), std::string("here"), std::string("line\nfeed"), std::string(1, static_cast<char>(0xFF)) }) {
        verb.title = invalid;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
    }
    verb.title.clear();
    CHECK(core.sendWorkspaceVerb(verb) != 0);
}

TEST(directTabMoveRequiresItsExactDestinationAndPlacement)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const UbiquittyTest::StructurePane source { 1, 7 };
    const UbiquittyTest::StructurePane destination { 9, 8 };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", { &source, 1 }, 17);
    auto other = UbiquittyTest::rowWorkspace(91, "Shared", { &destination, 1 }, 17);
    other.windows[0].id = Layout::SurfaceID { 2 };
    other.windows[0].isHostActive = false;
    other.windows[0].tabs[0].id = Layout::TabID { 2 };
    workspace.windows.push_back(std::move(other.windows[0]));
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::MoveTabToSurface;
    verb.surfaceID = 1;
    verb.tabID = 1;
    verb.targetPaneID = 9;
    for (unsigned fault = 0; fault < 3; ++fault) {
        const auto requestID = core.sendWorkspaceVerb(verb);
        REQUIRE(requestID);
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        const auto& parameters = *request.find("params");
        CHECK(request.stringOr("method") == "tab.move");
        CHECK(parameters.numberOr("tab") == 1 && parameters.numberOr("window") == 2 && parameters.numberOr("index") == 1);
        const auto result = fault == 0 ? R"({"tab":"tab:1","window":"window:3","index":1})"
            : fault == 1               ? R"({"tab":"tab:1","window":"window:2","index":0})"
                                       : R"({"tab":"tab:1","window":"window:2","index":1})";
        CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                  R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(","result":)" + result + "}")
            == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.flush());
        REQUIRE(!observer.m_workspaceResults.empty());
        CHECK(observer.m_workspaceResults.back().requestID == requestID);
        CHECK(observer.m_workspaceResults.back().wasPerformed == (fault == 2));
        CHECK(observer.m_workspaceResults.back().paneID == 0 && observer.m_workspaceResults.back().sessionID == 0);
    }
    CHECK(factory.hasCanonicalTranscript());
    verb.targetPaneID = 1;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.targetPaneID = 999;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.targetPaneID = 9;
    verb.tabID = 999;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
}

TEST(directSizingPinsInactiveTabAndExactSelectors)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    CHECK(core.workspaceMirrorGeneration() == 0);
    host.receive(factory.sentLog());
    const std::array panes { UbiquittyTest::StructurePane { 1, 7 }, UbiquittyTest::StructurePane { 2, 8 } };
    const std::array shown { UbiquittyTest::StructurePane { 3, 9 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", panes, 17, 19);
    auto tab = UbiquittyTest::rowWorkspace(91, {}, shown, 17).windows.front().tabs.front();
    tab.id = Layout::TabID { 2 };
    workspace.windows.front().tabs.push_back(std::move(tab));
    workspace.windows.front().hostActiveTab = 1;
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    const auto generation = core.workspaceMirrorGeneration();
    REQUIRE(generation && core.workspace());
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab, Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio, Ubiquitty::Session::WorkspaceIntentKind::SetDividerRatio }) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operation;
        verb.surfaceID = verb.tabID = verb.paneID = 1;
        verb.splitIndex = static_cast<std::uint16_t>(core.workspace()->paneTree(Layout::SurfaceID { 1 }, Layout::TabID { 1 })->root());
        verb.direction = std::to_underlying(Panes::Direction::Right);
        verb.ratio = 0.25f;
        const auto method = operation == Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab ? "tab.balance" : operation == Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio ? "pane.adjustRatio"
                                                                                                                                                                                                  : "divider.setRatio";
        for (unsigned fault = 0; fault < 5; ++fault) {
            const auto identity = core.sendWorkspaceVerb(verb);
            REQUIRE(identity);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            const auto& parameters = *request.find("params");
            CHECK(request.stringOr("method") == method && parameters.numberOr("window") == 1 && parameters.numberOr("tab") == 1);
            CHECK(parameters.stringOr("expectedRevision") == "19");
            auto result = Value::object();
            result.set("window", Value::string(fault == 0 || (fault == 4 && operation == Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab) ? "window:2" : "window:1"));
            result.set("tab", Value::string(fault == 1 || (fault == 2 && operation == Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab) ? "tab:2" : "tab:1"));
            if (operation == Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio) {
                CHECK(parameters.numberOr("pane") == 1 && parameters.stringOr("direction") == "right" && parameters.numberOr("step") == 0.25);
                result.set("pane", Value::string("pane:1"));
                result.set("direction", Value::string(fault == 2 ? "left" : "right"));
                result.set("step", Value::number(fault == 4 ? 0.5 : 0.25));
            } else if (operation == Ubiquitty::Session::WorkspaceIntentKind::SetDividerRatio) {
                REQUIRE(parameters.find("path"));
                CHECK(parameters.find("path")->items().empty() && parameters.numberOr("ratio") == 0.25);
                auto path = Value::array();
                if (fault == 2)
                    path.push(Value::number(1));
                result.set("path", std::move(path));
                result.set("ratio", Value::number(fault == 4 ? 0.5 : 0.25));
            }
            auto answer = Value::object();
            answer.set("jsonrpc", Value::string("2.0"));
            answer.set("id", *request.find("id"));
            answer.set("result", std::move(result));
            CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            CHECK(observer.m_workspaceResults.back().requestID == identity && observer.m_workspaceResults.back().wasPerformed == (fault == 3));
        }
        verb.surfaceID = 999;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.surfaceID = 1;
        verb.tabID = 999;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.tabID = 1;
        if (operation != Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab) {
            for (const float invalid : { -0.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
                verb.ratio = invalid;
                CHECK(core.sendWorkspaceVerb(verb) == 0);
            }
            if (operation == Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio) {
                verb.ratio = 0;
                CHECK(core.sendWorkspaceVerb(verb) == 0);
            } else {
                verb.ratio = 0.25f;
                verb.tabID = 2; // A single-pane tab has no root divider.
                verb.splitIndex = static_cast<std::uint16_t>(core.workspace()->paneTree(Layout::SurfaceID { 1 }, Layout::TabID { 2 })->root());
                CHECK(core.sendWorkspaceVerb(verb) == 0);
            }
        }
    }
    ++workspace.revision;
    workspace.windows.front().hostActiveTab = 0;
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    CHECK(core.workspaceMirrorGeneration() != generation);
    CHECK(factory.hasCanonicalTranscript());
}

TEST(directWindowCreationPinsSourceAndReturnsAllMintedIdentities)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    const std::array panes { UbiquittyTest::StructurePane { 1, 7 } };
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), UbiquittyTest::rowWorkspace(91, "Shared", panes, 17, 19)));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddSurface;
    verb.surfaceID = verb.paneID = 1;
    for (unsigned fault = 0; fault < 8; ++fault) {
        const auto requestID = core.sendWorkspaceVerb(verb);
        REQUIRE(requestID);
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        bool isJSON = false; // bool-ok: parser output
        const auto request = Codec::JSON::parse(calls[0].text, isJSON);
        REQUIRE(isJSON && request.find("params"));
        const auto& params = *request.find("params");
        CHECK(request.stringOr("method") == "workspace.addWindow");
        CHECK(params.numberOr("window") == 1 && params.numberOr("pane") == 1 && params.stringOr("expectedRevision") == "19");
        auto result = Value::object();
        result.set("sourceWindow", Value::string(fault == 0 ? "window:99" : "window:1"));
        result.set("sourcePane", Value::string(fault == 1 ? "pane:99" : "pane:1"));
        result.set("window", Value::string(fault == 2 ? "window:0" : fault == 3 ? "window:1"
                                                                                : "window:2"));
        result.set("tab", Value::string(fault == 4 ? "pane:2" : "tab:2"));
        result.set("pane", Value::string(fault == 5 ? "pane:1" : "pane:2"));
        result.set("session", Value::string(fault == 6 ? "session:0" : "session:8"));
        auto answer = Value::object();
        answer.set("jsonrpc", Value::string("2.0"));
        answer.set("id", *request.find("id"));
        answer.set("result", std::move(result));
        CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.flush());
        REQUIRE(!observer.m_workspaceResults.empty());
        const auto& returned = observer.m_workspaceResults.back();
        CHECK(returned.requestID == requestID && returned.wasPerformed == (fault == 7));
        CHECK(returned.surfaceID == (fault == 7 ? 2u : 0u) && returned.tabID == (fault == 7 ? 2u : 0u)
            && returned.paneID == (fault == 7 ? 2u : 0u) && returned.sessionID == (fault == 7 ? 8u : 0u));
    }
    verb.surfaceID = 99;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.surfaceID = 1;
    verb.paneID = 99;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    CHECK(factory.hasCanonicalTranscript());
}

TEST(directTransfersPinBothContainersAndPlacement)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const std::array sourcePanes { UbiquittyTest::StructurePane { 1, 7 }, UbiquittyTest::StructurePane { 2, 8 } };
    const std::array targetPanes { UbiquittyTest::StructurePane { 3, 9 }, UbiquittyTest::StructurePane { 4, 10 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", sourcePanes, 17, 19);
    auto destination = UbiquittyTest::rowWorkspace(91, {}, targetPanes, 17).windows.front();
    destination.id = Layout::SurfaceID { 2 };
    destination.tabs.front().id = Layout::TabID { 2 };
    workspace.windows.push_back(std::move(destination));
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::MovePaneToTab, Ubiquitty::Session::WorkspaceIntentKind::MoveTabToPane, Ubiquitty::Session::WorkspaceIntentKind::MovePaneToNewTab }) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operation;
        verb.surfaceID = verb.tabID = verb.paneID = 1;
        verb.targetPaneID = 3;
        verb.targetTabID = 2;
        verb.orientation = std::to_underlying(Panes::Orientation::Horizontal);
        verb.side = std::to_underlying(Panes::InsertSide::Second);
        verb.insertTarget = std::to_underlying(Panes::InsertTarget::Root);
        const bool isTab = operation == Ubiquitty::Session::WorkspaceIntentKind::MoveTabToPane;
        const bool isNewTab = operation == Ubiquitty::Session::WorkspaceIntentKind::MovePaneToNewTab;
        for (unsigned fault = 0; fault < 7; ++fault) {
            const auto requestID = core.sendWorkspaceVerb(verb);
            REQUIRE(requestID);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            const auto& params = *request.find("params");
            CHECK(request.stringOr("method") == (isTab ? "tab.moveToPane" : isNewTab ? "pane.moveToNewTab"
                                                                                     : "pane.moveToTab"));
            CHECK(params.numberOr("window") == 1 && params.numberOr(isTab ? "tab" : "pane") == 1);
            CHECK(params.stringOr("expectedRevision") == "19");
            CHECK(params.numberOr(isNewTab ? "targetWindow" : "target") == (isNewTab ? 2 : 3));
            if (!isNewTab) {
                CHECK(params.stringOr("orientation") == "horizontal" && params.stringOr("side") == "after" && params.stringOr("insertTarget") == "root");
                CHECK(params.numberOr("tab") == (isTab ? 1 : 2));
            }
            auto result = Value::object();
            result.set("sourceWindow", Value::string(fault == 0 ? "window:2" : "window:1"));
            result.set("sourceTab", Value::string(fault == 1 ? "tab:2" : "tab:1"));
            result.set("window", Value::string(fault == 3 ? "window:1" : "window:2"));
            result.set("tab", Value::string(fault == 4 ? "tab:0" : isNewTab ? "tab:3"
                                                                            : "tab:2"));
            if (!isTab)
                result.set("pane", Value::string(fault == 2 || (isNewTab && fault == 5) ? "pane:2" : "pane:1"));
            if (!isNewTab) {
                result.set("target", Value::string(isTab && fault == 2 ? "pane:4" : "pane:3"));
                result.set("orientation", Value::string("horizontal"));
                result.set("side", Value::string("after"));
                result.set("insertTarget", Value::string(fault == 5 ? "pane" : "root"));
            }
            auto answer = Value::object();
            answer.set("jsonrpc", Value::string("2.0"));
            answer.set("id", *request.find("id"));
            answer.set("result", std::move(result));
            CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            const auto& returned = observer.m_workspaceResults.back();
            CHECK(returned.requestID == requestID && returned.wasPerformed == (fault == 6));
            CHECK(returned.paneID == 0 && returned.sessionID == 0 && returned.tabID == 0 && returned.surfaceID == 0);
        }
        verb.targetPaneID = 999;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.targetPaneID = 3;
        verb.surfaceID = 2;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.surfaceID = 1;
        if (!isNewTab) {
            verb.insertTarget = 255;
            CHECK(core.sendWorkspaceVerb(verb) == 0);
            verb.insertTarget = std::to_underlying(Panes::InsertTarget::Pane);
            verb.targetPaneID = 1;
            verb.targetTabID = 1;
            CHECK(core.sendWorkspaceVerb(verb) == 0);
        }
    }
    CHECK(factory.hasCanonicalTranscript());
}

TEST(directExtractionsPinSourceAndMintedDestination)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const std::array panes { UbiquittyTest::StructurePane { 1, 7 }, UbiquittyTest::StructurePane { 2, 8 } };
    const auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", panes, 17, 9);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToTab, Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToSurface, Ubiquitty::Session::WorkspaceIntentKind::ExtractTabToSurface }) {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = operation;
        verb.surfaceID = verb.tabID = verb.paneID = 1;
        const bool isTab = operation == Ubiquitty::Session::WorkspaceIntentKind::ExtractTabToSurface;
        const bool isSameWindow = operation == Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToTab;
        for (unsigned fault = 0; fault < 6; ++fault) {
            const auto requestID = core.sendWorkspaceVerb(verb);
            REQUIRE(requestID);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            const auto& params = *request.find("params");
            CHECK(request.stringOr("method") == (isTab ? "tab.extractWindow" : isSameWindow ? "pane.extractTab"
                                                                                            : "pane.extractWindow"));
            CHECK(params.numberOr("window") == 1 && params.numberOr(isTab ? "tab" : "pane") == 1);
            CHECK(params.stringOr("expectedRevision") == "9");
            auto result = Value::object();
            result.set("sourceWindow", Value::string(fault == 0 ? "window:99" : "window:1"));
            result.set("window", Value::string(fault == 3 ? (isSameWindow ? "window:5" : "window:1") : isSameWindow ? "window:1"
                                                                                                                    : "window:5"));
            result.set("tab", Value::string(fault == 4 ? "tab:0" : isTab ? (fault == 1 || fault == 2 ? "tab:99" : "tab:1")
                                                                         : "tab:6"));
            if (!isTab) {
                result.set("pane", Value::string(fault == 1 ? "pane:2" : "pane:1"));
                result.set("sourceTab", Value::string(fault == 2 ? "tab:99" : "tab:1"));
            }
            auto answer = Value::object();
            answer.set("jsonrpc", Value::string("2.0"));
            answer.set("id", *request.find("id"));
            answer.set("result", std::move(result));
            CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            const auto& returned = observer.m_workspaceResults.back();
            CHECK(returned.requestID == requestID && returned.wasPerformed == (fault == 5));
            CHECK(returned.paneID == 0 && returned.sessionID == 0);
            CHECK(returned.tabID == (fault == 5 && isSameWindow ? 6u : 0u));
            CHECK(returned.surfaceID == (fault == 5 && !isSameWindow ? 5u : 0u));
        }
        verb.surfaceID = 99;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.surfaceID = 1;
        verb.tabID = verb.paneID = 99;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
    }
    CHECK(factory.hasCanonicalTranscript());
}

TEST(directPaneRelocationsPinBothTargetsAndRevision)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const std::array panes { UbiquittyTest::StructurePane { 1, 7 }, UbiquittyTest::StructurePane { 2, 8 } };
    const auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", panes, 17, 9);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.surfaceID = 1;
    verb.paneID = 1;
    verb.targetPaneID = 2;
    verb.orientation = std::to_underlying(Panes::Orientation::Horizontal);
    verb.side = std::to_underlying(Panes::InsertSide::First);
    for (const auto operation : { Ubiquitty::Session::WorkspaceIntentKind::MovePane, Ubiquitty::Session::WorkspaceIntentKind::SwapPanes }) {
        verb.verb = operation;
        for (unsigned fault = 0; fault < 4; ++fault) {
            const auto requestID = core.sendWorkspaceVerb(verb);
            REQUIRE(requestID);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            const auto& parameters = *request.find("params");
            CHECK(request.stringOr("method") == (operation == Ubiquitty::Session::WorkspaceIntentKind::MovePane ? "pane.move" : "pane.swap"));
            CHECK(parameters.numberOr("window") == 1 && parameters.numberOr("pane") == 1 && parameters.numberOr("target") == 2);
            CHECK(parameters.stringOr("expectedRevision") == "9");
            auto result = Value::object();
            result.set("pane", Value::string(fault == 2 ? "pane:2" : "pane:1"));
            result.set("target", Value::string(fault == 0 ? "pane:1" : "pane:2"));
            result.set("window", Value::string(fault == 1 ? "window:2" : "window:1"));
            if (operation == Ubiquitty::Session::WorkspaceIntentKind::MovePane) {
                CHECK(parameters.stringOr("orientation") == "horizontal" && parameters.stringOr("side") == "before");
                result.set("orientation", Value::string("horizontal"));
                result.set("side", Value::string("before"));
            }
            auto answer = Value::object();
            answer.set("jsonrpc", Value::string("2.0"));
            answer.set("id", *request.find("id"));
            answer.set("result", std::move(result));
            CHECK(host.plane().connection().answerCall(calls[0].callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            CHECK(observer.m_workspaceResults.back().requestID == requestID);
            CHECK(observer.m_workspaceResults.back().wasPerformed == (fault == 3));
            CHECK(observer.m_workspaceResults.back().paneID == 0 && observer.m_workspaceResults.back().sessionID == 0);
        }
        verb.targetPaneID = 1;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.targetPaneID = 999;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.targetPaneID = 2;
        verb.surfaceID = 99;
        CHECK(core.sendWorkspaceVerb(verb) == 0);
        verb.surfaceID = 1;
    }
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::MovePane;
    verb.side = 255;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    verb.side = std::to_underlying(Panes::InsertSide::Second);
    verb.orientation = 255;
    CHECK(core.sendWorkspaceVerb(verb) == 0);
    CHECK(factory.hasCanonicalTranscript());
}

TEST(directNavigationPinsTargetsAndStateDerivedRevisions)
{
    using Codec::JSON::Value;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    const std::array panes { UbiquittyTest::StructurePane { 1, 7 }, UbiquittyTest::StructurePane { 2, 8 } };
    auto workspace = UbiquittyTest::rowWorkspace(91, "Shared", panes, 17, 19);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(7, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7, 17, 91));
    host.receive(factory.sentLog());
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    struct Navigation {
        Ubiquitty::Session::WorkspaceIntentKind verb;
        const char* method;
        const char* result;
    };
    for (const auto& entry : std::array {
             Navigation { Ubiquitty::Session::WorkspaceIntentKind::SetActiveSurface, "window.activate", R"({"window":"window:1"})" },
             Navigation { Ubiquitty::Session::WorkspaceIntentKind::SetActiveTab, "tab.activate", R"({"window":"window:1","tab":"tab:1"})" },
             Navigation { Ubiquitty::Session::WorkspaceIntentKind::FocusPane, "pane.focus", R"({"window":"window:1","pane":"pane:1"})" },
             Navigation { Ubiquitty::Session::WorkspaceIntentKind::MoveFocus, "window.moveFocus", R"({"window":"window:1","fromPane":"pane:1","pane":"pane:2","direction":"right"})" },
             Navigation { Ubiquitty::Session::WorkspaceIntentKind::ToggleZoom, "pane.zoom", R"({"pane":"pane:1","state":"zoomed"})" } }) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            Ubiquitty::Session::WorkspaceIntent verb;
            verb.verb = entry.verb;
            verb.surfaceID = verb.tabID = verb.paneID = 1;
            verb.direction = std::to_underlying(Panes::Direction::Right);
            const auto requestID = core.sendWorkspaceVerb(verb);
            REQUIRE(requestID);
            host.receive(factory.sentLog());
            const auto calls = host.plane().connection().takeCalls();
            REQUIRE(calls.size() == 1);
            bool isJSON = false; // bool-ok: parser output
            const auto request = Codec::JSON::parse(calls[0].text, isJSON);
            REQUIRE(isJSON && request.find("params"));
            CHECK(request.stringOr("method") == entry.method);
            const auto& params = *request.find("params");
            if (entry.verb == Ubiquitty::Session::WorkspaceIntentKind::ToggleZoom || entry.verb == Ubiquitty::Session::WorkspaceIntentKind::MoveFocus)
                CHECK(params.stringOr("expectedRevision") == "19");
            if (entry.verb == Ubiquitty::Session::WorkspaceIntentKind::ToggleZoom)
                CHECK(params.stringOr("state") == "zoomed");
            if (entry.verb == Ubiquitty::Session::WorkspaceIntentKind::MoveFocus)
                CHECK(params.numberOr("pane") == 1 && params.numberOr("window") == 1 && params.stringOr("direction") == "right");
            const auto suffix = fault == 0 ? R"("result":{})" : fault == 1 ? R"("error":{"code":-32001,"message":"refresh"})"
                                                                           : std::string(R"("result":)") + entry.result;
            CHECK(host.plane().connection().answerCall(calls[0].callIndex,
                      R"({"jsonrpc":"2.0","id":")" + request.stringOr("id") + R"(",)" + suffix + "}")
                == Protocol::Sync::CallAnswer::Sent);
            pumpData(factory.last(), dispatcher, host.flush());
            REQUIRE(!observer.m_workspaceResults.empty());
            CHECK(observer.m_workspaceResults.back().requestID == requestID);
            CHECK(observer.m_workspaceResults.back().wasPerformed == (fault == 2));
            CHECK(observer.m_workspaceResults.back().paneID == 0 && observer.m_workspaceResults.back().sessionID == 0);
            host.receive(factory.sentLog());
            CHECK(host.plane().connection().takeCalls().empty());
        }
    }
    workspace.revision = 20;
    workspace.windows[0].hostZoomedPane = TreeZoom { .tab = 0, .pane = 0 };
    REQUIRE(UbiquittyTest::offerStructure(host.plane().connection(), workspace));
    pumpData(factory.last(), dispatcher, host.flush());
    Ubiquitty::Session::WorkspaceIntent toggle;
    toggle.verb = Ubiquitty::Session::WorkspaceIntentKind::ToggleZoom;
    toggle.surfaceID = toggle.paneID = 1;
    REQUIRE(core.sendWorkspaceVerb(toggle));
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false; // bool-ok: parser output
    const auto request = Codec::JSON::parse(calls[0].text, isJSON);
    REQUIRE(isJSON && request.find("params"));
    CHECK(request.find("params")->stringOr("state") == "unzoomed");
    CHECK(request.find("params")->stringOr("expectedRevision") == "20");
    CHECK(factory.hasCanonicalTranscript());
}

// setSize before the ack is carried by the eventual Greet rather than sent as
// a Viewport for a pane the host has not bound; a size that did not change sends
// nothing at all; a degenerate size is refused.

/// The size a client states is its own drawable's, whatever grid the host
/// holds for the pane.
///
/// The host sizes a pane among every client showing it, so the grid its
/// frames carry need not be the one this client's drawable holds. A drawable
/// that comes to hold the very grid the host is at is still a change of this
/// client's size, and the host is told: one left believing the earlier size
/// would apply it the moment this client's typing made the size its to set.
TEST(aClientStatesItsOwnGridWhateverGridTheHostHolds)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 100, 30);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(7, 17, 30, 100);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    pumpData(factory.last(), dispatcher, host.publishAt(factory, 30, 100));
    // Nothing is stated for a pane whose frontend has reported no size: it is
    // not laid out, and the grid it was started with is nobody's drawable.
    check(factory.sentViewports().empty(), "a size was stated before the frontend reported one");
    core.setSize(100, 30);
    const auto bound = factory.lastViewport();
    check(factory.sentViewports().size() == 1 && bound && bound->columns == 100 && bound->rows == 30,
        "the first report stated the grid this client holds");

    // Another client's size applies: the host's frames carry a grid this
    // client never named, and this client says nothing about it.
    const std::size_t statedBefore = factory.sentViewports().size();
    pumpData(factory.last(), dispatcher, host.publishAt(factory, 24, 80));
    check(term->columns == 80 && term->rows == 24, "the mirror took the host's grid");
    check(factory.sentViewports().size() == statedBefore, "a host frame at another grid made this client state a size");
    check(core.trace().totals().viewportsStated == statedBefore, "the trace counts the statements the wire carried");

    // The drawable comes to hold the host's grid: stated, because it is this
    // client's size that moved.
    core.setSize(80, 24);
    const auto followed = factory.lastViewport();
    check(factory.sentViewports().size() == statedBefore + 1 && followed && followed->columns == 80 && followed->rows == 24,
        "a drawable that came to hold the host's grid was not stated");

    // The same grid again states nothing, and neither does a host frame.
    core.setSize(80, 24);
    pumpData(factory.last(), dispatcher, host.publishAt(factory, 30, 100));
    core.setSize(80, 24);
    check(factory.sentViewports().size() == statedBefore + 1, "an unchanged size was stated again");

    // Back to the first grid: a change, so stated.
    core.setSize(100, 30);
    const auto back = factory.lastViewport();
    check(factory.sentViewports().size() == statedBefore + 2 && back && back->columns == 100, "the resize back was not stated");
}

/// A width change is the Viewport that asks for it and nothing after: the
/// history is the client's to rewrap (Docs/sync.md §6.6), so a settled
/// resize asks the host for no checkpoint, no history and no other geometry,
/// however long the window then waits.
TEST(aResizeSendsNothingUpstream)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 100, 30);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(7, 17, 30, 100);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    pumpData(factory.last(), dispatcher, host.publishAt(factory, 30, 100));

    core.setSize(120, 30);
    pumpData(factory.last(), dispatcher, host.publishAt(factory, 30, 120));
    check(term->columns == 120, "the mirror took the host's answer");
    const std::size_t resizes = factory.sentViewports().size();
    const std::size_t resyncs = factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::Resync);
    const std::size_t wants = factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::HistoryWant);
    // Every timer the core armed fires, a dozen times over: far more than
    // two hundred milliseconds of a 60 Hz client's clock.
    for (int round = 0; round < 12; ++round)
        dispatcher.fireDelayed();
    check(factory.sentViewports().size() == resizes, "a settled resize sent another Viewport");
    check(factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::Resync) == resyncs, "a settled resize asked for a checkpoint");
    check(factory.sentScreenFramesOfKind(Protocol::Sync::FrameKind::HistoryWant) == wants, "a settled resize asked for history");
}

TEST(test_set_size_gating_and_degenerate_sizes)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

    core.setSize(0, 24); // no columns
    core.setSize(80, 0); // no rows
    core.setSize(-1, -1);
    check(factory.sentCount() == 0, "a degenerate size sends nothing");

    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    core.setSize(100, 30); // not acked yet, and the HELLO already went out at 80x24
    check(factory.sentViewports().empty(), "nothing is asked before the handshake");
    SessionHost host(7, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));

    pumpData(factory.last(), dispatcher, host.publish(factory));
    const auto asked = factory.lastViewport();
    check(asked && asked->columns == 100 && asked->rows == 30,
        "a resize made after the Greet went out is asked for once the host has bound the pane");

    core.setSize(120, 40);
    const auto changed = factory.lastViewport();
    check(changed && changed->columns == 120 && changed->rows == 40, "a real change sends a Viewport naming it");
}

// start() twice, and the entry guards on the other public methods: a second
// start must not build a second channel over the first, which would leave the
// first one live with nothing owning it.
TEST(test_public_entry_guards)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

    core.start(0, 80, 24);
    const int afterFirst = factory.createCount();
    core.start(0, 80, 24);
    check(factory.createCount() == afterFirst, "a second start builds no second channel");

    // A reclaim before the ack is refused (it names a session id nobody has).
    CHECK(!core.requestReclaim());

    // deployFinished with no deploy pending is a no-op rather than a state move.
    const int statesBefore = observer.m_stateChanges;
    core.deployFinished(DeployResult::Succeeded, "");
    core.deployFinished(DeployResult::Failed, "no such thing");
    check(observer.m_stateChanges == statesBefore, "deployFinished with nothing pending changes no state");
}

// The two ways the platform can fail to give the core a channel. Both are
// first-class outcomes of the factory contract, and both must land in a state
// the UI can show rather than leaving the session waiting on a callback that
// will never come.
TEST(test_channel_build_and_start_failures)
{
    {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        factory.setBuildFails(true);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        dispatcher.pump();
        check(observer.m_state != State::Connected, "a channel that could not be built does not read as connected");
        check(observer.m_stateChanges > 0, "the failure reached the observer");
    }
    {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        factory.setStartFails(true);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        dispatcher.pump();
        check(observer.m_state != State::Connected, "a channel whose start refused does not read as connected");
    }
}

// The largest datagram the fixture's QUIC carrier sends, the size a QUIC path
// is guaranteed to carry.
constexpr std::size_t fixtureDatagramMaxBytes = 1200;

// Attaches the core to `host` over the stream and leaves the host holding the
// client's acknowledgement of its first checkpoint, so the updates after it
// travel as datagrams.
void attachForDatagrams(FakeFactory& factory, FakeDispatcher& dispatcher, SessionHost& host, std::uint32_t)
{
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    const std::vector<std::uint8_t> afterAck = host.publish(factory);
    if (!afterAck.empty())
        pumpData(factory.last(), dispatcher, afterAck);
    // The publish that read the acknowledgement already sent a datagram, of a
    // frame no different from the checkpoint; a case starts after it.
    (void)host.takeDatagrams();
}

// Writes `text` over the host's first row and returns the datagrams that carry
// the update — the probes the host sends beside them left out — with any
// stream bytes the host sent in `stream`.
std::vector<std::vector<std::uint8_t>> updateDatagrams(FakeFactory& factory, SessionHost& host, std::string_view text, std::vector<std::uint8_t>* stream = nullptr)
{
    Ubiquitty::processBytes(&host.terminal(), "\x1b[H\x1b[2K");
    Ubiquitty::processBytes(&host.terminal(), text);
    std::vector<std::uint8_t> carried = host.publish(factory);
    if (stream)
        *stream = std::move(carried);
    std::vector<std::vector<std::uint8_t>> updates;
    for (std::vector<std::uint8_t>& datagram : host.takeDatagrams()) {
        if (!datagram.empty() && datagram.front() == std::to_underlying(Protocol::Sync::FrameKind::Update))
            updates.push_back(std::move(datagram));
    }
    return updates;
}

// A datagram is the unreliable half of a QUIC channel, and on it the host sends
// screen updates. The core marshals a burst onto the session thread with one
// posted drain, applies it in arrival order, repaints once for it, and drops a
// burst that outlived its session rather than applying it.
TEST(test_datagram_ingress)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(9, 17, 24, 80, fixtureDatagramMaxBytes);
    attachForDatagrams(factory, dispatcher, host, 9);

    const std::vector<std::vector<std::uint8_t>> first = updateDatagrams(factory, host, "first update");
    const std::vector<std::vector<std::uint8_t>> second = updateDatagrams(factory, host, "second update");
    check(first.size() == 1 && second.size() == 1, "each update travelled as one datagram");
    if (first.size() != 1 || second.size() != 1)
        return;

    const int screensBefore = observer.m_screens;
    const std::size_t queuedBefore = dispatcher.queuedCount();
    factory.last()->fireDatagram(first.front());
    factory.last()->fireDatagram(second.front());
    check(dispatcher.queuedCount() == queuedBefore + 1, "one drain is posted for a datagram burst");
    dispatcher.pump();
    check(rowText(*term, 0).starts_with("second update"), "the burst applied in arrival order");
    check(observer.m_screens == screensBefore + 1, "and repainted once for the whole burst");

    // A datagram queued before a stop, delivered after: the generation guard
    // drops it rather than writing into a torn-down session.
    const std::vector<std::vector<std::uint8_t>> late = updateDatagrams(factory, host, "after the stop");
    check(late.size() == 1, "the late update travelled as one datagram");
    if (!late.empty())
        factory.last()->fireDatagram(late.front());
    core.stop();
    dispatcher.pump();
    check(rowText(*term, 0).starts_with("second update"), "a datagram from a stopped session is dropped");
}

// A datagram carries exactly one envelope. Bytes that are not one are a host
// speaking out of turn, and the connection ends rather than guessing where the
// envelope it meant begins.
TEST(test_a_datagram_that_is_not_one_envelope_ends_the_connection)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(9, 17, 24, 80, fixtureDatagramMaxBytes);
    attachForDatagrams(factory, dispatcher, host, 9);

    const std::vector<std::vector<std::uint8_t>> update = updateDatagrams(factory, host, "one");
    check(update.size() == 1, "the update travelled as one datagram");
    if (update.empty())
        return;
    std::vector<std::uint8_t> twoEnvelopes = update.front();
    twoEnvelopes.insert(twoEnvelopes.end(), update.front().begin(), update.front().end());
    factory.last()->fireDatagram(twoEnvelopes);
    dispatcher.pump();
    check(core.state() != State::Connected, "two envelopes in one datagram end the connection");
}

// Datagram ingress is bounded. A stalled session thread cannot let the I/O
// thread grow either byte or frame storage past the fixed batches; a datagram
// that does not fit is dropped, which costs the client only that update — the
// host resends what the client never acknowledges.
TEST(test_datagram_ingress_flood_is_bounded)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(9));

    using Access = Ubiquitty::Session::RemoteSessionDatagramIngressAccess;
    const std::size_t frameLimit = Access::batchMaximumFrames();
    const std::vector<std::uint8_t> smallest = sizedDatagramFrame(std::size_t { 3 });
    const std::size_t postsBeforeFrameFlood = dispatcher.postCount();
    for (std::size_t frame = 0; frame < frameLimit; ++frame)
        factory.last()->fireDatagram(smallest);
    check(Access::pendingFrames(core) == frameLimit, "the frame budget may fill exactly");
    factory.last()->fireDatagram(smallest);
    check(Access::pendingFrames(core) == frameLimit, "a datagram past the frame budget is dropped");
    check(Access::retainedFrames(core) <= Access::maxRetainedFrames(), "a frame flood cannot pass the aggregate retained budget");
    check(dispatcher.postCount() == postsBeforeFrameFlood + 1, "a stalled consumer still has one posted drain");
    check(core.state() == State::Connected, "and a dropped datagram does not end the connection");

    // Fill by bytes in a fresh session: what the flood above staged is not a
    // screen-plane envelope, and draining it would end that connection.
    core.stop();
    dispatcher.pump();
    auto byteTerm = makeTerm();
    FakeDispatcher byteDispatcher;
    FakeFactory byteFactory;
    RecordingObserver byteObserver;
    RemoteSessionCore byteCore(byteTerm.get(), &byteDispatcher, &byteFactory, &byteObserver);
    byteCore.start(0, 80, 24);
    pumpConnect(byteFactory.last(), byteDispatcher);
    pumpData(byteFactory.last(), byteDispatcher, helloAckFrame(9));
    const std::size_t byteLimit = Access::batchMaximumBytes();
    constexpr std::size_t stateFrameBytes = 1024;
    const std::vector<std::uint8_t> stateFrame = sizedDatagramFrame(stateFrameBytes);
    const std::size_t framesToFillBytes = byteLimit / stateFrame.size();
    for (std::size_t frame = 0; frame < framesToFillBytes; ++frame)
        byteFactory.last()->fireDatagram(stateFrame);
    check(Access::pendingBytes(byteCore) == framesToFillBytes * stateFrame.size(), "the byte budget accounts every staged frame");
    byteFactory.last()->fireDatagram(stateFrame);
    check(Access::pendingBytes(byteCore) <= byteLimit, "a byte flood cannot pass the pending-batch budget");
    check(Access::pendingFrames(byteCore) <= frameLimit, "the byte flood remains inside the pending-frame budget too");
    check(Access::retainedBytes(byteCore) <= Access::maxRetainedBytes(), "a byte flood cannot pass the aggregate retained budget");
    check(Access::storageBytes(byteCore) <= Access::maxStorageBytes(), "resident ingress storage is fixed at two bounded batches");
    byteCore.stop();
    byteDispatcher.pump();
}

// An observer that reacts to a repaint may start the next burst itself: a
// datagram arriving while the frontend repaints is drained, not stranded behind
// the drain that announced the repaint.
TEST(test_a_datagram_arriving_during_the_repaint_is_drained)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(9, 17, 24, 80, fixtureDatagramMaxBytes);
    attachForDatagrams(factory, dispatcher, host, 9);

    const std::vector<std::vector<std::uint8_t>> outer = updateDatagrams(factory, host, "outer");
    const std::vector<std::vector<std::uint8_t>> nested = updateDatagrams(factory, host, "nested");
    check(outer.size() == 1 && nested.size() == 1, "each update travelled as one datagram");
    if (outer.size() != 1 || nested.size() != 1)
        return;
    bool hasFired = false;
    observer.m_onScreen = [&] {
        if (std::exchange(hasFired, true))
            return;
        factory.last()->fireDatagram(nested.front());
        dispatcher.pump();
    };
    factory.last()->fireDatagram(outer.front());
    dispatcher.pump();
    observer.m_onScreen = {};
    check(rowText(*term, 0).starts_with("nested"), "the datagram that arrived during the repaint was applied");
}

// The I/O thread may refill one batch while the session thread consumes the
// other. The aggregate limit names that real peak, not only either half viewed
// alone, and another datagram cannot push the retained payload past it.
TEST(test_datagram_ingress_aggregate_bound_covers_both_batches)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(9));

    using Access = Ubiquitty::Session::RemoteSessionDatagramIngressAccess;
    const std::size_t frameLimit = Access::batchMaximumFrames();
    const std::size_t byteLimit = Access::batchMaximumBytes();
    const std::vector<std::uint8_t> smallest = sizedDatagramFrame(std::size_t { 3 });
    const std::size_t prefixBytes = (frameLimit - 1) * smallest.size();
    check(prefixBytes + std::size_t { 3 } <= byteLimit, "the exact-limit fixture has room for its final frame");
    const std::vector<std::uint8_t> finalFrame = sizedDatagramFrame(byteLimit - prefixBytes);
    const auto fillBatch = [&] {
        for (std::size_t frame = 0; frame < frameLimit - 1; ++frame)
            factory.last()->fireDatagram(smallest);
        factory.last()->fireDatagram(finalFrame);
    };

    fillBatch();
    check(Access::pendingBytes(core) == byteLimit && Access::pendingFrames(core) == frameLimit,
        "one ingress batch reaches both limits exactly");
    // The session thread takes the full batch to drain, and the producer
    // refills the other while it does.
    Access::beginDrain(core);
    fillBatch();
    const std::size_t retainedBytesAtPeak = Access::retainedBytes(core);
    const std::size_t retainedFramesAtPeak = Access::retainedFrames(core);
    factory.last()->fireDatagram(smallest);
    const std::size_t retainedBytesAfterOverflow = Access::retainedBytes(core);
    const std::size_t retainedFramesAfterOverflow = Access::retainedFrames(core);
    Access::endDrain(core);

    check(retainedBytesAtPeak == Access::maxRetainedBytes(), "the aggregate byte bound includes both full batches");
    check(retainedFramesAtPeak == Access::maxRetainedFrames(), "the aggregate frame bound includes both full batches");
    check(retainedBytesAfterOverflow <= Access::maxRetainedBytes(), "overflow cannot pass the aggregate retained-byte bound");
    check(retainedFramesAfterOverflow <= Access::maxRetainedFrames(), "overflow cannot pass the aggregate retained-frame bound");
    check(Access::storageBytes(core) <= Access::maxStorageBytes(), "concurrent refill still owns exactly two fixed batches");
    core.stop();
    dispatcher.pump();
}

// A new attempt may start while an old attempt's datagram drain is still in
// the dispatcher. Posted state is generation-scoped: the old drain cannot
// clear the flag for the new one and strand its bytes forever.
TEST(test_datagram_drain_post_is_generation_scoped)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost oldHost(9, 17, 24, 80, fixtureDatagramMaxBytes);
    attachForDatagrams(factory, dispatcher, oldHost, 9);

    FakeChannel* oldChannel = factory.last();
    const int createsBeforePathChange = factory.createCount();
    const std::vector<std::vector<std::uint8_t>> stale = updateDatagrams(factory, oldHost, "old generation");
    if (!stale.empty())
        oldChannel->fireDatagram(stale.front()); // old drain remains queued
    core.networkPathChanged(); // a stream connection is asked to answer on the new path first
    dispatcher.fireDelayed(); // it stays silent, so it is torn down and redialled
    FakeChannel* replacement = factory.last();
    check(replacement != nullptr && factory.createCount() == createsBeforePathChange + 1,
        "the path change created a replacement channel");
    if (!replacement)
        return;
    SessionHost newHost(9, 17, 24, 80, fixtureDatagramMaxBytes);
    newHost.passOver(factory);
    pumpConnect(replacement, dispatcher);
    attachForDatagrams(factory, dispatcher, newHost, 9);
    const std::vector<std::vector<std::uint8_t>> fresh = updateDatagrams(factory, newHost, "new generation");
    check(fresh.size() == 1, "the new generation's update travelled as one datagram");
    if (!fresh.empty())
        replacement->fireDatagram(fresh.front());

    dispatcher.pump();
    check(core.sessionID() == 9, "the replacement handshake preserves the exact retained session");
    check(rowText(*term, 0).starts_with("new generation"), "the new generation's datagram was not stranded by the old drain");
}

// Runs an idle attachment over a QUIC carrier for two minutes of host time:
// the host probes on its backing-off schedule as datagrams, and the core
// answers on whatever the carrier carries. A host that hears nothing for 30 s
// with a probe outstanding declares the path lost (Docs/sync.md §6.4), so a
// core whose `Echo` never leaves is dropped though nothing is wrong.
//
// - Parameter support: Whether the carrier sends the core's datagrams; one
//   that does not leaves the core the stream.
// - Returns: The host time at which the host declared the path lost, or 0.
std::uint64_t idleQUICAttachmentLostAt(DatagramSupport support)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    factory.last()->setDatagramSupport(support);
    SessionHost host(9, 17, 24, 80, fixtureDatagramMaxBytes);
    attachForDatagrams(factory, dispatcher, host, 9);
    const std::uint64_t quietSince = host.now();
    std::size_t datagramsRead = 0;
    while (host.now() - quietSince < 120'000'000) {
        dispatcher.fireDelayed();
        FakeChannel* channel = factory.last();
        if (!channel)
            break;
        const std::span<const std::vector<std::uint8_t>> sent = std::span { channel->sentDatagrams() }.subspan(datagramsRead);
        datagramsRead += sent.size();
        const std::vector<std::uint8_t> stream = host.idle(factory, sent, 250'000);
        if (host.connection().state() == Protocol::Sync::ConnectionState::Lost)
            return host.now() - quietSince;
        if (!stream.empty())
            pumpData(channel, dispatcher, stream);
        for (const std::vector<std::uint8_t>& datagram : host.takeDatagrams())
            channel->fireDatagram(datagram);
        dispatcher.pump();
    }
    check(host.connection().counters().probesSent >= 4, "the idle host probed on its schedule");
    return 0;
}

// An idle core on QUIC answers the host's datagram probes, so the host never
// takes a quiet, healthy path for a dead one. The answer travels as a datagram
// where the carrier takes one and on the stream where it does not.
TEST(anIdleQUICClientAnswersTheHostsDatagramProbesAndIsNeverLost)
{
    const std::uint64_t lostAt = idleQUICAttachmentLostAt(DatagramSupport::Accepted);
    CHECK_MSG(lostAt == 0, ("the host declared the idle path lost after " + std::to_string(lostAt) + " us").c_str());
    const std::uint64_t lostWithoutDatagrams = idleQUICAttachmentLostAt(DatagramSupport::Refused);
    CHECK_MSG(lostWithoutDatagrams == 0, ("a carrier refusing datagrams lost the idle path after " + std::to_string(lostWithoutDatagrams) + " us").c_str());
}

// The same staleness rule for the connect and data callbacks: the I/O thread
// can post either one before the session thread has run work that tears the
// channel down, and the marshaled handler must notice on arrival.
TEST(test_stale_connect_and_data_are_dropped)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(3, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setClock([&] { return host.plane().now(); });
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 3));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    for (unsigned round = 0; round < 4; ++round) {
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.flush());
        dispatcher.fireDelayed();
    }

    // Queue a data callback, stop, then drain: the frame is from a session
    // that no longer exists.
    factory.last()->fireData(bellFrame(host, factory));
    const int bells = observer.m_bells;
    core.stop();
    dispatcher.pump();
    check(observer.m_bells == bells, "data queued before a stop is dropped");

    // And a connect callback the same way.
    auto term2 = makeTerm();
    FakeDispatcher dispatcher2;
    FakeFactory factory2;
    RecordingObserver observer2;
    RemoteSessionCore core2(term2.get(), &dispatcher2, &factory2, &observer2);
    core2.start(0, 80, 24);
    factory2.last()->fireConnect();
    core2.stop();
    dispatcher2.pump();
    check(observer2.m_state == State::Idle || observer2.m_state != State::Connected,
        "a connect queued before a stop does not connect");
}

// A frame the decoder cannot trust at all: a header declaring a payload past
// the protocol's ceiling faults the decoder, and the session must treat that
// as a broken link rather than reading the next bytes as a fresh frame.
TEST(test_undecodable_frame_faults_the_session)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));

    // A known host kind with a nonminimal/out-of-range length is refused
    // from its direct header, not mistaken for an obsolete outer frame.
    pumpData(factory.last(), dispatcher, { std::to_underlying(Protocol::Sync::FrameKind::Notify), 0, 0xFF, 0xFF, 0xFF });
    check(observer.m_state != State::Connected, "an undecodable frame leaves the connected state");
}

// A reconnect scheduled by a drop must not fire once the session has been
// stopped in the meantime: the delayed callback re-checks, because the timer
// was armed before the user closed the window.
TEST(test_scheduled_reconnect_checks_again_when_it_fires)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(6));
    pumpClose(factory.last(), dispatcher, "dropped");
    check(dispatcher.hasDelayed(), "a drop arms the backoff timer");

    const int createsBeforeStop = factory.createCount();
    core.stop();
    dispatcher.fireDelayed();
    check(factory.createCount() == createsBeforeStop, "the timer does not reconnect a stopped session");
}

// The host->client pushes the core forwards untouched. Each is one line, and
// each is what a UI element reads: the window title, the directory in the tab
// label, the answer to a file request, the foreground-process signal the
// resize path consults. A push that stops being forwarded shows as a UI that
// silently stops updating, with no error anywhere.
TEST(test_host_pushes_reach_the_observer)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    UbiquittyTest::HostScreen host(31, 17);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 31));

    host.receive(factory.sentLog());
    Protocol::Sync::PaneFacts facts;
    facts.programTitle = "a new title";
    facts.currentDirectory = "/home/user";
    facts.processChain = "-zsh";
    facts.foreground.name = "zsh";
    facts.shellCommandName = "zsh";
    facts.isAtPrompt = true;
    host.plane().setFacts(facts);

    // One typed record supplies both the presentation and the resize gate.
    const int titlesBefore = observer.m_titles;
    pumpData(factory.last(), dispatcher, host.publish());
    check(observer.m_titles > titlesBefore, "PaneFacts reaches the title observer");
    check(core.foregroundIsShell(), "the canonical shell fact reaches the resize gate");
    facts.foreground.name = "less";
    facts.isAtPrompt = false;
    host.plane().setFacts(facts);
    pumpData(factory.last(), dispatcher, host.flush());
    check(!core.foregroundIsShell(), "a later canonical record disables pager reflow");

    // Every one of them landed on a live session rather than faulting it.
    check(core.state() == State::Connected, "the session is still connected after every push");
}

// A refusal the client cannot satisfy is a stopping point, not a slower loop.
// Version mismatch is the shape the design anticipates by construction — the
// app and the daemon auto-update independently — so the client that retries it
// asks a question whose answer cannot change, once a second, forever.
TEST(sshAuthenticationFailuresDoNotReconnect)
{
    for (const auto* reason : { "the connection closed: Permission denied (publickey).", "the connection closed: Host key verification failed.", "the connection closed: REMOTE HOST IDENTIFICATION HAS CHANGED" }) {
        auto terminal = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(terminal.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        pumpClose(factory.last(), dispatcher, reason);
        check(core.state() == State::Failed, "SSH authentication failures require user action");
        check(!dispatcher.hasDelayed(), "SSH authentication failures schedule no retry");
        core.stop();
    }
}

TEST(test_permanent_auth_denial_is_terminal)
{
    const Protocol::Sync::ByeReason permanent[] = {
        Protocol::Sync::ByeReason::VersionMismatch,
        Protocol::Sync::ByeReason::TokenInvalid,
        Protocol::Sync::ByeReason::TokenRevoked,
        Protocol::Sync::ByeReason::TokenMissing,
        Protocol::Sync::ByeReason::TokenExpired,
        Protocol::Sync::ByeReason::OutsideScope,
        Protocol::Sync::ByeReason::RemoteHostRefused,
    };
    for (const auto reason : permanent) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);

        // The daemon refuses the handshake, then drops us — the order a real
        // denial arrives in.
        pumpData(factory.last(), dispatcher, authDeniedFrame(reason));
        check(core.state() == State::Failed, "a permanent denial ends in Failed");
        check(!dispatcher.hasDelayed(), "a permanent denial schedules no reconnect");

        // The daemon's own close lands on an already-failed session and must
        // not resurrect the loop.
        if (factory.last())
            pumpClose(factory.last(), dispatcher, "daemon closed after denial");
        check(core.state() == State::Failed, "the daemon's close leaves it Failed");
        check(!dispatcher.hasDelayed(), "the close after a denial schedules no reconnect");
        check(factory.createCount() == 1, "no second attempt is ever built");
        term.reset();
    }
}

// The two reasons a retry can actually answer differently keep retrying: a
// second Greet on one connection is not what a fresh connection sends, and
// nobody-to-consent is a person who may walk up to the host.
TEST(test_transient_auth_denial_keeps_retrying)
{
    const Protocol::Sync::ByeReason transient[] = {
        Protocol::Sync::ByeReason::NoConsent,
        Protocol::Sync::ByeReason::DuplicateHandshake,
    };
    for (const auto reason : transient) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);

        factory.last()->fireData(authDeniedFrame(reason));
        queueClose(factory.last(), "daemon closed after denial");
        dispatcher.pump();
        check(core.state() != State::Failed, "a transient denial is not terminal");
        check(core.state() == State::Reconnecting, "a transient denial reconnects");
        check(dispatcher.hasDelayed(), "a transient denial schedules the retry");
        term.reset();
    }
}

// A host that is down is asked about less and less. The first attempts stay
// fast, because the common drop is an ssh blip that fixes itself; past those
// the delay doubles to a ceiling, and a handshake that lands puts it back.
TEST(test_reconnect_backoff_grows_then_resets)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));

    // The dispatcher also records screen/liveness ticks. Capture only the
    // new timer each drop schedules, not unrelated timers from a live link.
    std::vector<double> delays;
    // Ten drops in a row, each retry failing to reach a handshake.
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto timersBeforeDrop = dispatcher.delaysRequested().size();
        pumpClose(factory.last(), dispatcher, "host down");
        REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
        delays.push_back(dispatcher.delaysRequested().back());
        dispatcher.fireDelayed();
        CHECK_EQ(factory.createCount(), attempt + 2);
        pumpConnect(factory.last(), dispatcher);
    }
    check(delays.size() == 10, "one delay was requested per attempt");
    check(delays[0] == 1.0 && delays[1] == 1.0 && delays[2] == 1.0, "the first three retries stay fast");
    check(delays[3] > delays[2], "the delay grows once the fast attempts are spent");
    for (std::size_t i = 1; i < delays.size(); ++i)
        check(delays[i] >= delays[i - 1], "the schedule never shortens while failing");
    check(delays.back() <= 30.0, "the backoff is capped");
    check(delays.back() > delays[3], "the cap is reached by growing, not by starting there");

    // A handshake lands: the next drop starts fast again, because the host
    // being reachable is evidence the previous run of failures is over.
    pumpData(factory.last(), dispatcher, helloAckFrame(5));
    check(core.state() == State::Connected, "the retry finally connected");
    const auto timersBeforeDrop = dispatcher.delaysRequested().size();
    pumpClose(factory.last(), dispatcher, "another blip");
    REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
    check(dispatcher.delaysRequested().back() == 1.0, "a landed handshake resets the backoff");

    term.reset();
}

// A host that drops a client for falling behind its output says so, and the
// client shows it rather than an empty "Reconnecting". Reconnecting at once
// re-enters the backlog that caused the drop, so each such drop in a row waits
// longer, and a handshake landing in between does not reset that: the host
// being reachable is exactly the condition under which it overflowed. Any
// other drop starts the schedule over.
TEST(test_backlog_drop_names_itself_and_backs_off)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));
    REQUIRE(core.state() == State::Connected);

    const std::string notice(Protocol::Sync::byeNotice(Protocol::Sync::ByeReason::OutputBacklog));
    std::vector<double> delays;
    for (int drop = 0; drop < 4; ++drop) {
        const auto timersBeforeDrop = dispatcher.delaysRequested().size();
        factory.last()->fireData(authDeniedFrame(Protocol::Sync::ByeReason::OutputBacklog));
        queueClose(factory.last(), "");
        dispatcher.pump();
        CHECK(core.state() == State::Reconnecting);
        CHECK_STR_EQ(observer.m_lastReason, notice);
        REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
        delays.push_back(dispatcher.delaysRequested().back());
        // The wait is the reconnect: nothing dials before it fires.
        CHECK_EQ(factory.createCount(), drop + 1);
        dispatcher.fireDelayed();
        CHECK_EQ(factory.createCount(), drop + 2);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, helloAckFrame(5));
        REQUIRE(core.state() == State::Connected);
    }
    CHECK_MSG(delays[0] > Ubiquitty::Session::reconnectDelaySeconds, "the first backlog drop reconnected on the ordinary fast schedule");
    for (std::size_t i = 1; i < delays.size(); ++i)
        CHECK_MSG(delays[i] > delays[i - 1], "a second backlog drop in a row did not wait longer than the first");
    CHECK(delays.back() <= Ubiquitty::Session::reconnectMaxDelaySeconds);

    // An ordinary drop is not a backlog: the schedule starts fast again, and
    // so does the next backlog drop.
    auto timersBeforeDrop = dispatcher.delaysRequested().size();
    pumpClose(factory.last(), dispatcher, "another blip");
    REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
    CHECK(dispatcher.delaysRequested().back() == Ubiquitty::Session::reconnectDelaySeconds);
    dispatcher.fireDelayed();
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));
    timersBeforeDrop = dispatcher.delaysRequested().size();
    factory.last()->fireData(authDeniedFrame(Protocol::Sync::ByeReason::OutputBacklog));
    queueClose(factory.last(), "");
    dispatcher.pump();
    REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
    CHECK(dispatcher.delaysRequested().back() == delays.front());

    term.reset();
}

// A QUIC host cannot put a Bye behind the backlog it is dropping the client
// for, so it closes the connection with the reason in its CONNECTION_CLOSE.
// The client reads it into the same notice and the same longer wait a Bye on
// the stream would have given it; a close without one stays the plain drop.
TEST(test_quic_backlog_close_names_itself_and_backs_off)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(5));
    REQUIRE(core.state() == State::Connected);

    const std::string notice(Protocol::Sync::byeNotice(Protocol::Sync::ByeReason::OutputBacklog));
    std::vector<double> delays;
    for (int drop = 0; drop < 2; ++drop) {
        const auto timersBeforeDrop = dispatcher.delaysRequested().size();
        factory.last()->setApplicationCloseCode(Protocol::Sync::quicCloseCodeOf(Protocol::Sync::ByeReason::OutputBacklog));
        pumpClose(factory.last(), dispatcher, "");
        CHECK(core.state() == State::Reconnecting);
        CHECK_STR_EQ(observer.m_lastReason, notice);
        REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
        delays.push_back(dispatcher.delaysRequested().back());
        dispatcher.fireDelayed();
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, helloAckFrame(5));
        REQUIRE(core.state() == State::Connected);
    }
    CHECK_MSG(delays[0] > Ubiquitty::Session::reconnectDelaySeconds, "a QUIC backlog close reconnected on the ordinary fast schedule");
    CHECK_MSG(delays[1] > delays[0], "a second QUIC backlog close in a row did not wait longer");

    // An application code that names no reason is an ordinary drop.
    const auto timersBeforeDrop = dispatcher.delaysRequested().size();
    factory.last()->setApplicationCloseCode(Protocol::Sync::quicByeCloseCodeBase - 1);
    pumpClose(factory.last(), dispatcher, "closed");
    REQUIRE(dispatcher.delaysRequested().size() == timersBeforeDrop + 1);
    CHECK(dispatcher.delaysRequested().back() == Ubiquitty::Session::reconnectDelaySeconds);
    CHECK_STR_EQ(observer.m_lastReason, "closed");

    term.reset();
}

// One hop per burst, not per chunk — and the bytes still arrive in order and
// whole, which is the only thing the decoder cares about.
TEST(test_inbound_chunks_batch_into_one_hop)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(3, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setClock([&] { return host.plane().now(); });
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 3));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    for (unsigned round = 0; round < 4; ++round) {
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.flush());
        dispatcher.fireDelayed();
    }

    // A frame split across many arrivals, the shape a flood takes: the I/O
    // thread hands over bytes faster than the session thread drains them.
    const auto frame = bellFrame(host, factory);
    REQUIRE(!frame.empty());
    const std::size_t postsBefore = dispatcher.postCount();
    for (const std::uint8_t byte : frame)
        factory.last()->fireData(std::vector<std::uint8_t> { byte });
    check(dispatcher.postCount() == postsBefore + 1, "a burst of chunks posts one drain");
    dispatcher.pump();
    check(observer.m_bells == 1, "the split frame decoded whole, in order");

    // With the drain no longer in flight, the next arrival posts again.
    factory.last()->fireData(bellFrame(host, factory));
    check(dispatcher.postCount() == postsBefore + 2, "a later burst posts its own drain");
    dispatcher.pump();
    check(observer.m_bells == 2, "the second frame landed too");

    term.reset();
}

// ---- roaming: the device moves to another network ---------------------

/// A path change retimes a backoff the old path earned.
///
/// The wait after a failed redial describes the network the redial failed on. Once the device is on
/// a different one it describes nothing, and a session that sat out the remaining delay would stay
/// down for up to the ceiling after the network it needed was already there.
TEST(aNetworkChangeRedialsInsteadOfWaitingOutTheBackoff)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(core.state() == State::Connected, "the session reached Connected");

    // Drive the backoff out past its flat head so the pending wait is a long one.
    for (int attempt = 0; attempt < 6; ++attempt) {
        pumpClose(factory.last(), dispatcher, "drop");
        dispatcher.fireDelayed();
    }
    check(core.state() != State::Connected, "the session is down after the drops");

    // The last close leaves a timer pending. The device moves.
    pumpClose(factory.last(), dispatcher, "drop");
    check(core.state() == State::Reconnecting, "a drop leaves the session reconnecting");
    const int createsBeforeChange = factory.createCount();
    core.networkPathChanged();
    dispatcher.pump();

    check(factory.createCount() > createsBeforeChange,
        "the path change redialled rather than waiting out the delay");

    // The superseded timer must not open a second channel beside the redial.
    const int createsAfterRedial = factory.createCount();
    dispatcher.fireDelayed();
    check(factory.createCount() == createsAfterRedial,
        "the timer the path change superseded still fired a connect");
}

/// A path change may accelerate one attempt, but cannot erase failed attempts.
TEST(aNetworkChangePreservesTheAttemptCount)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));

    for (int attempt = 0; attempt < 6; ++attempt) {
        pumpClose(factory.last(), dispatcher, "drop");
        dispatcher.fireDelayed();
    }
    pumpClose(factory.last(), dispatcher, "drop");
    check(dispatcher.delaysRequested().back() > 1.0, "the backoff had grown past its flat head");
    const double previousDelay = dispatcher.delaysRequested().back();

    core.networkPathChanged();
    dispatcher.pump();
    // The redial fails too; the delay it asks for next is what this asserts.
    pumpClose(factory.last(), dispatcher, "drop");
    check(dispatcher.delaysRequested().back() >= previousDelay,
        "a network notification must not erase the accumulated failures");
}

/// A live QUIC session is left alone, because QUIC migrates.
///
/// Tearing it down would throw away a working connection on every hop between networks — the exact
/// case QUIC's connection migration exists to survive.
TEST(aNetworkChangeLeavesALiveQUICSessionAlone)
{
    WelcomedSession session(TransportMode::QUIC);
    check(session.core->state() == State::Connected, "the QUIC session reached Connected");
    check(session.core->isUsingQUIC(), "and is on QUIC");

    const int createsBefore = session.factory.createCount();
    session.core->networkPathChanged();
    session.dispatcher.pump();

    check(session.core->state() == State::Connected, "a QUIC session survived the path change");
    check(session.factory.createCount() == createsBefore, "and did not redial");
}

/// A path change asks a live QUIC session to prove it migrated.
///
/// Migration is QUIC's promise, not a fact: a tunnel that stays up but stops
/// passing traffic, or a host that went away without a word, leaves the
/// session showing Connected until the idle timer runs out a minute and a half
/// later, with every keystroke typed meanwhile lost. The ask is the screen
/// plane's Probe, before the carrier is trusted again (Docs/sync.md §6.9).
TEST(aPathChangeProbesBeforeTrustingTheCarrier)
{
    WelcomedSession session(TransportMode::QUIC);
    const int createsBefore = session.factory.createCount();
    const std::size_t probesBefore = session.probes();

    session.core->networkPathChanged();
    check(session.probes() == probesBefore + 1, "the host was probed on the new path");
    check(session.core->state() == State::Connected && session.factory.createCount() == createsBefore, "the connection is kept while it is asked");
    session.answer();
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore, "a connection that answered on the new path is kept");

    session.dispatcher.pump();
    session.core->networkPathChanged();
    check(session.probes() == probesBefore + 2, "the second move was probed as the first was");
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore + 1, "a connection silent on the new path is replaced");
}

TEST(resumingAfterSuspensionReattachesALiveQUICSession)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(true);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    const int createsBefore = factory.createCount();
    core.resumeAfterSuspension(120.0);
    check(factory.createCount() == createsBefore + 1, "foreground resume obtains a fresh carrier");
    check(core.state() == State::Connecting, "resume waits for a real handshake");
    check(core.sessionID() == 7, "resume retains the session identity");
    dispatcher.fireDelayed();
    check(factory.createCount() == createsBefore + 1, "resume does not schedule a duplicate carrier");
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    check(core.state() == State::Connected, "the same session reconnects");
    core.stop();
    core.resumeAfterSuspension(120.0);
    check(factory.createCount() == createsBefore + 1, "resume leaves an explicitly closed session stopped");
}

TEST(resumingAfterSuspensionInvalidatesTheBackoffTimer)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpClose(factory.last(), dispatcher, "suspended");
    const int createsBefore = factory.createCount();
    core.resumeAfterSuspension(120.0);
    check(factory.createCount() == createsBefore + 1, "resume immediately retries a dropped carrier");
    dispatcher.fireDelayed();
    check(factory.createCount() == createsBefore + 1, "the abandoned backoff timer cannot redial");
}

TEST(aHandingOverByeRedialsAtOnceUnpinned)
{
    for (const auto carrier : { TransportMode::TCP, TransportMode::QUIC }) {
        WelcomedSession session(carrier, "predecessor.sock", 17);
        session.deliverUnreadOutput("kept picture");
        const auto before = UbiquittyTest::checkpointBody(*session.term);
        const int creates = session.factory.createCount();
        const auto delays = session.dispatcher.delaysRequested().size();
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = Protocol::Sync::ByeReason::HandingOver, .detail = {} });
        std::vector<std::uint8_t> envelope;
        REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Bye, 0, body));
        const auto frame = envelope;
        session.factory.last()->fireData(frame);
        queueClose(session.factory.last(), "predecessor closed");
        session.dispatcher.pump();
        CHECK(session.factory.lastCommand().find(reattachSocketToken) != std::string::npos);
        CHECK_EQ(session.factory.createCount(), creates + 1);
        CHECK_EQ(session.factory.handoverClears(), 1);
        CHECK_EQ(session.dispatcher.delaysRequested().size(), delays);
        CHECK(session.core->state() == State::Connecting);
        CHECK_EQ(session.core->sessionID(), 7u);
        CHECK(UbiquittyTest::checkpointBody(*session.term) == before);
        const auto sent = session.factory.sentCount();
        CHECK(session.core->send(Protocol::Sync::RawInput { .bytes = { 'x' } }));
        CHECK_EQ(session.core->inputStatus().waiting, 1u);
        CHECK_EQ(session.factory.sentCount(), sent);
        session.dispatcher.fireDelayed();
        CHECK_EQ(session.factory.createCount(), creates + 1);
        pumpConnect(session.factory.last(), session.dispatcher);
        const auto hello = session.factory.nthOfType(helloType, 1);
        REQUIRE(hello);
        const auto decoded = Protocol::Sync::decodeGreet(*hello);
        REQUIRE(decoded);
        CHECK_EQ(decoded->request.expectedDaemonInstanceID, 17u);
        CHECK_EQ(decoded->request.sessionID, 7u);
    }
}

TEST(anOrdinaryTransportDropKeepsTheDaemonPin)
{
    WelcomedSession session(TransportMode::TCP, "predecessor.sock");
    const int creates = session.factory.createCount();
    pumpClose(session.factory.last(), session.dispatcher, "link lost");
    CHECK_EQ(session.factory.createCount(), creates);
    CHECK_EQ(session.factory.handoverClears(), 0);
    session.dispatcher.fireDelayed();
    CHECK_EQ(session.factory.createCount(), creates + 1);
    CHECK(session.factory.lastCommand().find("SOCK=\"predecessor.sock\";") != std::string::npos);
}

// The pinned daemon stopped and another took in its records. The pinned
// bootstrap says the daemon is gone; the client drops the pin and asks the
// host's current daemon at once, for the same instance and session, and does
// so once: a second report before any daemon answered is a failure, not a
// loop.
TEST(aPinnedDaemonReportedGoneIsAskedForAgainAtTheHostsCurrentDaemonOnce)
{
    WelcomedSession session(TransportMode::TCP, "host-c756f263bc841e61.sock", 17);
    pumpClose(session.factory.last(), session.dispatcher, "link lost");
    session.dispatcher.fireDelayed();
    CHECK(session.factory.lastCommand().find("SOCK=\"host-c756f263bc841e61.sock\";") != std::string::npos);
    pumpConnect(session.factory.last(), session.dispatcher);
    const int creates = session.factory.createCount();
    const auto delays = session.dispatcher.delaysRequested().size();
    pumpData(session.factory.last(), session.dispatcher, sentinelBytes("\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\n"));
    CHECK_EQ(session.factory.handoverClears(), 1);
    CHECK_EQ(session.factory.createCount(), creates + 1);
    CHECK_EQ(session.dispatcher.delaysRequested().size(), delays);
    CHECK(session.factory.lastCommand().find(reattachSocketToken) != std::string::npos);
    CHECK(session.core->state() == State::Connecting);
    pumpConnect(session.factory.last(), session.dispatcher);
    const auto hello = session.factory.nthOfType(helloType, 2);
    REQUIRE(hello);
    const auto decoded = Protocol::Sync::decodeGreet(*hello);
    REQUIRE(decoded);
    CHECK_EQ(decoded->request.expectedDaemonInstanceID, 17u);
    CHECK_EQ(decoded->request.sessionID, 7u);

    pumpData(session.factory.last(), session.dispatcher, sentinelBytes("\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\n"));
    CHECK(session.core->state() == State::Failed);
    CHECK_EQ(session.factory.createCount(), creates + 1);
    CHECK_EQ(session.dispatcher.delaysRequested().size(), delays);
    CHECK(!session.dispatcher.hasDelayed());
}

// A QUIC bootstrap pinned to a stopped daemon builds no carrier and says why:
// neither the same pin's stream nor a timed retry is tried.
TEST(aQUICBootstrapThatReportsItsDaemonGoneIsNotRetriedOrSentToTheStream)
{
    WelcomedSession session(TransportMode::QUIC, "host-4c6dfcc7ad63d335.sock", 17);
    pumpClose(session.factory.last(), session.dispatcher, "link lost");
    session.factory.failNextCreation({ .recovery = ChannelFailureRecovery::InstanceGone, .detail = "gone" });
    const int creates = session.factory.createCount();
    session.dispatcher.fireDelayed();
    CHECK_EQ(session.factory.createCount(), creates + 2);
    CHECK_EQ(session.factory.handoverClears(), 1);
    CHECK(session.factory.lastPreferQUIC());
    CHECK(session.factory.lastCommand().find(reattachSocketToken) != std::string::npos);
    CHECK(session.core->state() == State::Connecting);
}

// The host's current daemon holds no record of the workspace: the person is
// told so, and nothing dials again.
TEST(aCurrentDaemonWithoutTheWorkspaceEndsTheReresolutionPlainly)
{
    WelcomedSession session(TransportMode::TCP, "host-c756f263bc841e61.sock", 17);
    pumpClose(session.factory.last(), session.dispatcher, "link lost");
    session.dispatcher.fireDelayed();
    pumpConnect(session.factory.last(), session.dispatcher);
    pumpData(session.factory.last(), session.dispatcher, sentinelBytes("\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\n"));
    pumpConnect(session.factory.last(), session.dispatcher);
    const int creates = session.factory.createCount();
    pumpData(session.factory.last(), session.dispatcher, authDeniedFrame(Protocol::Sync::ByeReason::SessionGone));
    if (session.factory.last())
        pumpClose(session.factory.last(), session.dispatcher, "refused");
    CHECK(session.core->state() == State::Failed);
    CHECK(session.core->lastRefusal().detail == "Ubiquitty restarted on the host, and the saved workspace is no longer available.");
    CHECK(!session.dispatcher.hasDelayed());
    CHECK_EQ(session.factory.createCount(), creates);
}

// A QUIC close that carries a permanent refusal is the host's answer, as the
// same Bye on the stream is: the session fails and is not redialled.
TEST(aQUICCloseCarryingAVersionRefusalIsTerminal)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    factory.setOfferQUIC(true);
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    factory.last()->setApplicationCloseCode(Protocol::Sync::quicByeCloseCodeBase + std::to_underlying(Protocol::Sync::ByeReason::VersionMismatch));
    pumpClose(factory.last(), dispatcher, "closed");
    CHECK(core.state() == State::Failed);
    CHECK(core.lastRefusal().hasRefusal && core.lastRefusal().reason == Protocol::Sync::ByeReason::VersionMismatch);
    CHECK(!dispatcher.hasDelayed());
    CHECK_EQ(factory.createCount(), 1);
    core.stop();
}

TEST(aSuccessorWelcomePinsOnlyItsValidatedIdentityAndRetainsTheSession)
{
    for (const auto carrier : { TransportMode::TCP, TransportMode::QUIC }) {
        for (const bool hasMatchingPredecessor : { false, true }) {
            WelcomedSession session(carrier, "predecessor.sock", 17);
            session.deliverUnreadOutput("kept picture");
            const auto before = UbiquittyTest::checkpointBody(*session.term);
            pumpClose(session.factory.last(), session.dispatcher, "carrier lost");
            session.dispatcher.fireDelayed();
            pumpConnect(session.factory.last(), session.dispatcher);
            const auto creates = session.factory.createCount();
            Protocol::Sync::Welcome welcome;
            welcome.wireVersion = Protocol::Sync::wireVersion;
            welcome.hostInstanceID = { 22, 0 };
            welcome.attachmentID = { 2, 22 };
            welcome.resumeSecret = { 3, 22 };
            welcome.selection = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = 7, .daemonInstanceID = 22, .socketName = "successor.sock", .hostPlatform = "fixture" };
            welcome.predecessorHostInstanceID = Protocol::Sync::ID128 { hasMatchingPredecessor ? 17u : 18u, 0 };
            welcome.resumed = { { .paneID = { 7, 17 }, .outcome = Protocol::Sync::ResumeOutcome::Diffed, .inputWritten = 0 } };
            std::vector<std::uint8_t> body;
            Protocol::Sync::encode(body, welcome);
            std::vector<std::uint8_t> envelope;
            REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Welcome, 0, body));
            // An incomplete authenticated selection must publish neither its
            // daemon/socket nor any following presentation or pane frames.
            REQUIRE(envelope.size() > 1);
            const auto titles = session.observer.m_titles;
            pumpData(session.factory.last(), session.dispatcher, { envelope.begin(), envelope.end() - 1 });
            CHECK(session.core->state() == State::Connecting);
            CHECK(!session.core->acknowledged());
            CHECK_EQ(session.core->daemonInstanceID(), 17u);
            CHECK_EQ(session.observer.m_titles, titles);
            CHECK_EQ(session.factory.handoverClears(), 0);
            CHECK(session.core->send(Protocol::Sync::RawInput { .bytes = { 'x' } }));
            std::vector<std::uint8_t> frame { envelope.back() };
            envelope.clear();
            body.clear();
            Protocol::Sync::encode(body, Protocol::Sync::PaneBind { .handle = 11, .paneID = { 7, 17 } });
            REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::PaneBind, 0, body));
            frame.insert(frame.end(), envelope.begin(), envelope.end());
            const auto title = titleFrame("confirmed successor", 11);
            frame.insert(frame.end(), title.begin(), title.end());
            pumpData(session.factory.last(), session.dispatcher, frame);
            CHECK(UbiquittyTest::checkpointBody(*session.term) == before);
            CHECK_EQ(session.core->sessionID(), 7u);
            if (!hasMatchingPredecessor) {
                CHECK(session.core->state() == State::Failed);
                CHECK_EQ(session.observer.m_titles, titles);
                CHECK_EQ(session.factory.handoverClears(), 0);
                session.dispatcher.fireDelayed();
                CHECK_EQ(session.factory.createCount(), creates);
                continue;
            }
            CHECK(session.core->state() == State::Connected);
            CHECK(session.core->acknowledged());
            CHECK_EQ(session.core->daemonInstanceID(), 22u);
            CHECK_EQ(session.factory.handoverClears(), 1);
            CHECK(session.observer.m_failureReasons.empty());
            // The stable pane still belongs to host 17, but file calls must
            // use its handle on host 22's admitted physical connection.
            const auto requestID = session.core->requestFile("/srv/file");
            REQUIRE(requestID);
            body.clear();
            Protocol::Sync::encode(body, Protocol::Sync::Result { .transfer = 0, .text = R"({"jsonrpc":"2.0","id":"file:1","result":{"pane":11,"path":"/srv/file","status":"ok","bytes":3,"data":"AP9B"}})" });
            envelope.clear();
            REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Result, 0, body));
            frame = envelope;
            pumpData(session.factory.last(), session.dispatcher, frame);
            REQUIRE(session.observer.m_files.size() == 1);
            CHECK_EQ(session.observer.m_files.front().requestID, requestID);
            CHECK(session.observer.m_files.front().status == Protocol::Control::FileReadStatus::OK);
            CHECK(std::ranges::equal(session.observer.m_files.front().data(), std::array<std::uint8_t, 3> { 0, 255, 'A' }));
            pumpClose(session.factory.last(), session.dispatcher, "second drop");
            session.dispatcher.fireDelayed();
            CHECK(session.factory.lastCommand().find("SOCK=\"successor.sock\";") != std::string::npos);
        }
    }
}

TEST(aPaneUnboundForItsSizeFailsWithTheReasonAndRecoversWhenBoundAgain)
{
    WelcomedSession session(TransportMode::TCP);
    REQUIRE(session.core->state() == State::Connected);
    auto& connection = session.host->connection();
    connection.unbindPane(session.host->paneHandle(), std::to_underlying(Protocol::Sync::PaneUnbindReason::CheckpointTooLarge));
    pumpData(session.factory.last(), session.dispatcher, session.host->idle(session.factory, {}, 0));
    // A failure with its reason, not the window closing.
    CHECK(session.core->state() == State::Failed);
    REQUIRE(!session.observer.m_failureReasons.empty());
    CHECK(session.observer.m_failureReasons.back().find("checkpoint limit") != std::string::npos);
    CHECK_EQ(session.observer.m_exits, 0);
    // Bound again once it fits: no longer failed.
    REQUIRE(connection.bindPane(session.host->source()) != 0);
    pumpData(session.factory.last(), session.dispatcher, session.host->idle(session.factory, {}, 0));
    CHECK(session.core->state() == State::Connected);
    CHECK_EQ(session.observer.m_exits, 0);
}

TEST(aScreenNotificationRetiresTheOldHandoverBeforeItCanRedial)
{
    for (const bool replace : { false, true }) {
        WelcomedSession session(TransportMode::QUIC, "predecessor.sock", 17);
        Ubiquitty::processBytes(&session.host->terminal(), "final predecessor output");
        auto frame = session.host->publishUnread();
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = Protocol::Sync::ByeReason::HandingOver, .detail = {} });
        std::vector<std::uint8_t> envelope;
        REQUIRE(Protocol::Sync::appendFrame(envelope, Protocol::Sync::FrameKind::Bye, 0, body));
        const auto bye = envelope;
        frame.insert(frame.end(), bye.begin(), bye.end());
        const int creates = session.factory.createCount();
        const int screens = session.observer.m_screens;
        FakeFactory replacementFactory;
        REQUIRE(replacementFactory.pinDaemonSocket("replacement.sock"));
        bool wasNotified = false;
        session.observer.m_onScreen = [&] {
            REQUIRE(!wasNotified);
            wasNotified = true;
            session.core.reset();
            if (!replace)
                return;
            session.core.emplace(session.term.get(), &session.dispatcher, &replacementFactory, &session.observer);
            session.core->start(7, 80, 24);
        };
        pumpData(session.factory.last(), session.dispatcher, frame);
        CHECK(wasNotified);
        CHECK_EQ(session.observer.m_screens, screens + 1);
        CHECK_EQ(session.factory.handoverClears(), 0);
        CHECK_EQ(session.factory.createCount(), creates);
        CHECK_EQ(replacementFactory.createCount(), replace ? 1 : 0);
        CHECK_EQ(replacementFactory.handoverClears(), 0);
        if (replace) {
            REQUIRE(session.core);
            CHECK(session.core->state() == State::Connecting);
            CHECK(replacementFactory.lastCommand().find("SOCK=\"replacement.sock\";") != std::string::npos);
        }
        session.core.reset();
    }
}

/// A short suspension keeps a live QUIC connection once the host answers.
///
/// The connection outlives a switch to another app: the host's idle timer has
/// most of its window left. Throwing it away on every foreground forced a fresh
/// SSH bootstrap on port 22 at the moment the path was least ready for one.
TEST(aShortSuspensionKeepsAQUICConnectionTheHostStillAnswers)
{
    WelcomedSession session(TransportMode::QUIC);
    const int createsBefore = session.factory.createCount();
    const std::size_t probesBefore = session.probes();

    session.core->resumeAfterSuspension(5.0);
    check(session.factory.createCount() == createsBefore, "a short suspension opened no new carrier");
    check(session.core->state() == State::Connected, "the session stayed connected through the check");
    check(session.probes() == probesBefore + 1, "the host was asked for an answer");

    session.answer();
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore, "an answering host keeps its connection");
    check(session.core->state() == State::Connected, "and the session stays connected");
}

/// A short suspension redials when the kept connection does not answer.
TEST(aShortSuspensionRedialsWhenTheHostDoesNotAnswer)
{
    WelcomedSession session(TransportMode::QUIC);
    const int createsBefore = session.factory.createCount();

    session.core->resumeAfterSuspension(5.0);
    check(session.factory.createCount() == createsBefore, "the connection was replaced before it was asked");
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore + 1, "a silent connection is replaced");
    check(session.core->state() == State::Connecting, "the replacement waits for its handshake");
    check(session.core->sessionID() == 7, "and keeps the session identity");
}

/// Only the host's answer to the ask proves a kept connection.
///
/// Frames the host sent before it read the ask, queued while the process was
/// suspended or while the old path was up, arrive first. Counting them as
/// proof kept a connection whose path was dead, which then showed Connected
/// and swallowed keystrokes until the host's idle timer ran out.
TEST(framesSentBeforeTheAskDoNotProveAKeptConnection)
{
    WelcomedSession session(TransportMode::QUIC);
    const int createsBefore = session.factory.createCount();

    session.core->resumeAfterSuspension(5.0);
    pumpData(session.factory.last(), session.dispatcher, titleFrame("printed while suspended"));
    session.deliverUnreadOutput("printed while suspended\r\n");
    check(session.factory.createCount() == createsBefore, "the connection was replaced before its deadline");
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore + 1, "output queued before the ask kept a connection that never answered it");

    session.attach();
    session.core->networkPathChanged();
    session.deliverUnreadOutput("printed on the old path\r\n");
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore + 2, "output queued before a path change kept a silent connection");
}

/// A suspension past the host's idle window redials at once.
TEST(aLongSuspensionRedialsWithoutAsking)
{
    for (const double secondsSuspended : { 120.0, -1.0 }) {
        WelcomedSession session(TransportMode::QUIC);
        const int createsBefore = session.factory.createCount();
        const std::size_t probesBefore = session.probes();

        session.core->resumeAfterSuspension(secondsSuspended);
        check(session.factory.createCount() == createsBefore + 1, "a long or unmeasured suspension replaces the connection at once");
        check(session.probes() == probesBefore, "nothing was asked of the reaped connection");

        session.dispatcher.pump();
        session.core->resumeAfterSuspension(secondsSuspended);
        check(session.factory.createCount() == createsBefore + 1, "a repeated wake must let the bootstrap finish");
    }
}

/// A live non-migrating session that is silent on the new path is torn down and redialled.
///
/// A connection bound to an address the device no longer holds is already dead; the carrier just
/// will not say so until its own timeout runs. It cannot answer the host's proof of life, so the
/// deadline for that answer is what replaces it.
TEST(aNetworkChangeRedialsALiveNonMigratingSession)
{
    WelcomedSession session(TransportMode::TCP);
    check(session.core->state() == State::Connected, "the session reached Connected");
    check(!session.core->isUsingQUIC(), "and is not on QUIC");

    const int createsBefore = session.factory.createCount();
    session.core->networkPathChanged();
    session.dispatcher.pump();
    check(session.factory.createCount() == createsBefore, "the connection was replaced before it was asked");
    session.dispatcher.fireDelayed();

    check(session.factory.createCount() > createsBefore,
        "a non-migrating session redialled after the path change");
}

/// A live stream session is asked to answer after a wake or a path change.
///
/// The TCP connection under ssh survives a short sleep and every move that
/// leaves its own route alone, such as a VPN tunnel coming up beside it.
/// Replacing it on each one forced a fresh login and lost the input typed in
/// the gap.
TEST(aStreamSessionThatAnswersIsKeptThroughAWakeAndAPathChange)
{
    WelcomedSession session(TransportMode::TCP);
    const int createsBefore = session.factory.createCount();

    session.core->resumeAfterSuspension(5.0);
    check(session.factory.createCount() == createsBefore, "a wake tore down a stream connection before asking it");
    session.answer();
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore && session.core->state() == State::Connected, "a stream connection that answered after a wake was replaced");

    session.dispatcher.pump();
    session.core->networkPathChanged();
    check(session.factory.createCount() == createsBefore, "a path change tore down a stream connection before asking it");
    session.answer();
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore && session.core->state() == State::Connected, "a stream connection that answered after a path change was replaced");

    session.dispatcher.pump();
    session.core->networkPathChanged();
    session.dispatcher.fireDelayed();
    check(session.factory.createCount() == createsBefore + 1, "a stream connection silent after a path change was kept");
}

// A session that reattaches names its session before the host has answered,
// but not the daemon instance that, with it, is the pane: input is not taken
// until the host has named that, or it would wait for a pane no host binds.
TEST(inputIsNotTakenBeforeTheHostNamesTheDaemonInstance)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(7, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    check(!core.send(Protocol::Sync::RawInput { .bytes = { 'x' } }), "input was taken for a pane whose daemon instance is unknown");
    SessionHost host(7, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    check(core.send(Protocol::Sync::RawInput { .bytes = { 'x' } }), "input was refused once the host had named the pane");
    pumpData(factory.last(), dispatcher, host.publish(factory));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    (void)host.publish(factory);
    check(host.typed().bytes() == "x", "the input taken while the pane was unbound was not written once it was");
}

// A focus the frontend reported before the host named the session is the
// pane's once it is named.
TEST(aFocusReportedBeforeTheHandshakeReachesThePane)
{
    namespace Sync = Protocol::Sync;
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.setSurfaceFocus(Modes::FocusState::Focused);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    SessionHost host(7, 17, 24, 80);
    pumpData(factory.last(), dispatcher, host.welcome(factory));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    pumpData(factory.last(), dispatcher, host.publish(factory));
    (void)host.publish(factory);
    const auto isFocus = [](const Sync::InputIntent& intent) { return std::holds_alternative<Sync::FocusInput>(intent); };
    check(std::ranges::count_if(host.typed().events(), isFocus) == 1, "the pane was not told the focus reported before the handshake");
}

// A surface's focus is its pane's at the host, which reports it to the
// program under DECSET 1004: the pane is told when the frontend says, told
// again on the connection that follows a drop, and a frontend that never
// says leaves the pane unfocused there.
TEST(aSurfacesFocusIsItsPanesFocusAtTheHost)
{
    namespace Sync = Protocol::Sync;
    const auto focusEvents = [](SessionHost& host) {
        std::vector<Modes::FocusState> events;
        for (const Sync::InputIntent& intent : host.typed().events()) {
            if (const auto* focus = std::get_if<Sync::FocusInput>(&intent))
                events.push_back(focus->focus);
        }
        return events;
    };
    WelcomedSession session(TransportMode::TCP);
    session.answer();
    CHECK_MSG(focusEvents(*session.host).empty(), "a pane whose frontend said nothing was told it is focused");

    session.core->setSurfaceFocus(Modes::FocusState::Focused);
    session.answer();
    CHECK(focusEvents(*session.host) == std::vector { Modes::FocusState::Focused });
    const std::size_t writes = session.factory.sentCount();
    session.core->setSurfaceFocus(Modes::FocusState::Focused);
    CHECK_MSG(session.factory.sentCount() == writes, "the same focus again was written again");
    session.answer();
    CHECK_MSG(focusEvents(*session.host).size() == 1, "the same focus again reached the program again");
    session.core->setSurfaceFocus(Modes::FocusState::Unfocused);
    session.answer();
    CHECK(focusEvents(*session.host) == (std::vector { Modes::FocusState::Focused, Modes::FocusState::Unfocused }));

    // The next connection's host holds no focus for the pane: it is told.
    session.core->setSurfaceFocus(Modes::FocusState::Focused);
    session.answer();
    pumpClose(session.factory.last(), session.dispatcher, "drop");
    session.dispatcher.fireDelayed();
    session.attach();
    session.answer();
    CHECK(focusEvents(*session.host) == std::vector { Modes::FocusState::Focused });
}

/// A connection whose screen plane the host has not welcomed cannot be asked
/// for a proof of life, and a wake or a path change replaces it at once.
TEST(aConnectionThatCannotBeAskedIsReplacedUnasked)
{
    for (const bool isWake : { true, false }) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        check(core.state() == State::Connecting && !core.acknowledged(), "Greet alone grants no connection authority");
        const int createsBefore = factory.createCount();
        if (isWake)
            core.resumeAfterSuspension(5.0);
        else
            core.networkPathChanged();
        check(factory.createCount() == createsBefore + 1, "an unwelcomed connection was kept unasked");
    }
}

/// A session still waiting for its Welcome holds a carrier dialled over the
/// path that just went away, and that carrier reports the loss no sooner than
/// a live one would; it redials on the new path as a connected one does.
TEST(aNetworkChangeRedialsASessionStillConnecting)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    factory.setOfferQUIC(false);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    check(core.state() == State::Connecting, "premise: Greet is out and unanswered");

    const int createsBefore = factory.createCount();
    core.networkPathChanged();
    dispatcher.pump();

    check(factory.createCount() == createsBefore + 1, "the connecting session redialled at once");
    dispatcher.fireDelayed();
    check(factory.createCount() == createsBefore + 1, "and no backoff timer opens a second one");
}

/// A stopped session stays stopped.
///
/// A platform may report a path change at any time, including after the person closed the window;
/// reconnecting one that was explicitly ended would resurrect it.
TEST(aNetworkChangeDoesNotResurrectAStoppedSession)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    core.stop();
    dispatcher.pump();

    const int createsBefore = factory.createCount();
    core.networkPathChanged();
    dispatcher.pump();
    check(factory.createCount() == createsBefore, "a stopped session redialled on a path change");
}

TEST(clearBufferIsSentToAHostThatAdvertisedIt)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(1, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 1));
    CHECK(!core.sendClearBuffer(Ubiquitty::Screen::ClearCommand::Buffer));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    for (const auto command : { Screen::ClearCommand::Buffer, Screen::ClearCommand::Scrollback, Screen::ClearCommand::Screen, Screen::ClearCommand::ToPreviousMark })
        CHECK(core.sendClearBuffer(command));
    CHECK(!core.sendClearBuffer(static_cast<Screen::ClearCommand>(4)));
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 4);
    std::uint64_t priorID = 0;
    const std::array names { "buffer", "scrollback", "screen", "previous-mark" };
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto request = Protocol::Control::decodeRequest(calls[index].text, {});
        CHECK(request.method == "pane.clear");
        CHECK(request.params.numberOr("pane") == 1);
        CHECK(request.params.stringOr("command") == names[index]);
        CHECK(request.id.asNumber() > priorID);
        priorID = static_cast<std::uint64_t>(request.id.asNumber());
    }
    CHECK(factory.hasCanonicalTranscript());

    term.reset();
}

// Docs/session-model.md rule 5: the mirror of a hosted workspace is the host's
// to write. A frontend reads it and cannot adopt an inner mux's ids into it,
// because the only handle it gets is read-only.
static_assert(std::is_same_v<decltype(std::declval<const Ubiquitty::Session::RemoteSessionCore&>().workspace()), const Ubiquitty::Session::Layout::Workspace*>);

// ---- residue ledger: Session/RemoteSessionCore.cpp -------------------
// What stays dark after these cases, and why:
//
// - transition()'s same-state early return: it is private with ten call sites
//   in this file, each of which is reached only on a real state change.
// - writeFrame's null-channel half: all nine call sites either check the
//   channel first or run only past the ack, when one exists by construction.
// - handleClose's second null-channel guard: the only two sites that null the
//   channel are teardownChannel, which clears the observer with it, and stop,
//   which latches m_stopped — so a close cannot arrive with a null channel and
//   a live generation.
// - The deploy-sentinel tail beyond the prefix match, and the two
//   reconnect-scheduling guards, remain for a follow-up: they need a
//   byte-exact sentinel fixture rather than a new contract.
// - stageGenerationLocked's discard of a previous attempt's staged bytes: a
//   serial dispatcher runs the drain its chunk posted before any work queued
//   later, so the staging buffer is empty by the time a new attempt can append
//   to it, and handleData's generation guard drops those bytes rather than
//   feeding them. The discard is what keeps that true if the buffer ever does
//   survive a bump; it is defence in depth, and a case asserting it passes
//   with the discard removed, which is a case that proves nothing.

TEST(sharedWorkspacePublicationWaitsForConfirmationAndRetriesAfterDisconnect)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(7, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    SharedWorkspace shared;
    WorkspaceWindow window;
    window.tabs.push_back({ "shell", PaneNode::makeLeaf({}), 0 });
    shared.arrangement.windows.push_back(std::move(window));
    shared.sessions = { 7 };
    CHECK(!core.publishSharedWorkspace(shared));
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    CHECK(core.publishSharedWorkspace(shared));
    CHECK(core.sharedWorkspaceResult().isPending);
    CHECK(!core.publishSharedWorkspace(shared));
    host.receive(factory.sentLog());
    auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
    CHECK(request.method == "workspace.share");
    CHECK(request.params.stringOr("daemonInstanceID") == "17");
    CHECK(request.id.asString() == "ubiquitty.share.7");
    const auto parsed = readSharedWorkspace(request.params);
    REQUIRE(parsed);
    CHECK(parsed->sessions == shared.sessions);
    CHECK(host.plane().connection().answerCall(calls.front().callIndex,
              R"({"jsonrpc":"2.0","id":"ubiquitty.share.7","result":{"workspace":19}})")
        == Protocol::Sync::CallAnswer::Sent);
    pumpData(factory.last(), dispatcher, host.publish());
    CHECK(!core.sharedWorkspaceResult().isPending);
    CHECK_EQ(core.sharedWorkspaceResult().workspaceID, 19u);
    CHECK(core.sharedWorkspaceResult().detail.empty());
    CHECK(core.publishSharedWorkspace(shared));
    pumpClose(factory.last(), dispatcher, "lost before confirmation");
    CHECK(!core.sharedWorkspaceResult().isPending);
    CHECK_EQ(core.sharedWorkspaceResult().workspaceID, 0u);
    CHECK(!core.sharedWorkspaceResult().detail.empty());
    CHECK(!core.publishSharedWorkspace(shared));
    CHECK(core.reconnectNow());
    UbiquittyTest::HostScreen reconnectedHost(7, 17);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(reconnectedHost, factory, 7));
    reconnectedHost.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, reconnectedHost.publish());
    CHECK(core.publishSharedWorkspace(shared));
    reconnectedHost.receive(factory.sentLog());
    const auto retry = reconnectedHost.plane().connection().takeCalls();
    REQUIRE(retry.size() == 1);
    CHECK(retry.front().text == calls.front().text);
}

TEST(sharedWorkspacePublicationRejectsMalformedAndRefusedAnswers)
{
    for (const auto* result : { R"("result":{"workspace":0})", R"("result":{"workspace":1.5})", R"("result":{"workspace":"7"})",
             R"("error":{"code":-32002,"message":"denied"})", R"("error":{},"result":{"workspace":7})" }) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        UbiquittyTest::HostScreen host(7, 17);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.publish());
        SharedWorkspace shared;
        WorkspaceWindow window;
        window.tabs.push_back({ "shell", PaneNode::makeLeaf({}), 0 });
        shared.arrangement.windows.push_back(std::move(window));
        shared.sessions = { 8 };
        CHECK(!core.publishSharedWorkspace(shared));
        shared.sessions = { 7 };
        CHECK(core.publishSharedWorkspace(shared));
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        const auto answer = std::string(R"({"jsonrpc":"2.0","id":"ubiquitty.share.7",)") + result + "}";
        CHECK(host.plane().connection().answerCall(calls.front().callIndex, answer) == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.publish());
        CHECK(!core.sharedWorkspaceResult().isPending);
        CHECK_EQ(core.sharedWorkspaceResult().workspaceID, 0u);
        CHECK(core.publishSharedWorkspace(shared));
    }
}

TEST(clipboardRoutingChangesEstablishedConnectionWithoutReconnect)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(7, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    CHECK(!core.canChangeClipboardServing());
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));
    // This control call belongs to the authenticated physical client, not a
    // pane: Welcome admits it even before the first PaneBind arrives.
    CHECK(core.canChangeClipboardServing());
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    CHECK(core.canChangeClipboardServing());
    CHECK(!core.isClipboardRoutingPending());
    CHECK(!core.didClaimClipboardServe());
    const auto before = factory.sentCount();
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Enabled));
    CHECK(!core.didClaimClipboardServe());
    CHECK(core.isClipboardRoutingPending());
    CHECK(factory.sentCount() == before + 1);
    CHECK(factory.hasCanonicalTranscript());
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 1);
    const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
    CHECK(request.method == "client.clipboardServing");
    CHECK(request.params.boolOr("enabled"));
    CHECK(request.params.members().size() == 1);
    CHECK(host.plane().connection().answerCall(calls.front().callIndex,
              R"({"jsonrpc":"2.0","id":)" + std::to_string(static_cast<std::uint64_t>(request.id.asNumber())) + R"(,"result":{"enabled":true}})")
        == Protocol::Sync::CallAnswer::Sent);
    const auto acknowledgement = host.publish();
    pumpData(factory.last(), dispatcher, acknowledgement);
    CHECK(core.didClaimClipboardServe());
    CHECK(!core.isClipboardRoutingPending());
    CHECK(observer.m_clipboardRoutingChanges == 1);
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Enabled));
    CHECK(factory.sentCount() == before + 1);
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Disabled));
    CHECK(!core.didClaimClipboardServe());
    CHECK(factory.sentCount() == before + 2);
    pumpData(factory.last(), dispatcher, acknowledgement);
    CHECK(!core.didClaimClipboardServe());
    CHECK(factory.createCount() == 1);
}

TEST(clipboardRoutingWaitsForTheLatestTypedAnswer)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    UbiquittyTest::HostScreen host(7, 17);
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));
    host.receive(factory.sentLog());
    pumpData(factory.last(), dispatcher, host.publish());
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Enabled));
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Disabled));
    CHECK(!core.didClaimClipboardServe());
    CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Enabled));
    host.receive(factory.sentLog());
    const auto calls = host.plane().connection().takeCalls();
    REQUIRE(calls.size() == 3);
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto request = Protocol::Control::decodeRequest(calls[index].text, {});
        const auto answer = R"({"jsonrpc":"2.0","id":)" + std::to_string(static_cast<std::uint64_t>(request.id.asNumber()))
            + R"(,"result":{"enabled":)" + (request.params.boolOr("enabled") ? "true" : "false") + "}}";
        CHECK(host.plane().connection().answerCall(calls[index].callIndex, answer) == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.publish());
        CHECK(core.didClaimClipboardServe() == (index == 2));
        CHECK(observer.m_clipboardRoutingChanges == (index == 2 ? 1 : 0));
    }
}

TEST(clipboardRoutingRefusesMalformedOrUncorrelatedResults)
{
    for (const auto* suffix : { R"(,"result":{"enabled":1}})", R"(,"result":{"enabled":false}})", R"(,"result":{"enabled":true,"client":7}})", R"(,"error":{"code":-32002,"message":"denied"}})", R"(,"error":{},"result":{"enabled":true}})" }) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        UbiquittyTest::HostScreen host(7, 17);
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, welcomeOnly(host, factory, 7));
        host.receive(factory.sentLog());
        pumpData(factory.last(), dispatcher, host.publish());
        CHECK(core.setClipboardServing(Ubiquitty::Clipboard::Serving::Enabled));
        host.receive(factory.sentLog());
        const auto calls = host.plane().connection().takeCalls();
        REQUIRE(calls.size() == 1);
        const auto request = Protocol::Control::decodeRequest(calls.front().text, {});
        const auto answer = R"({"jsonrpc":"2.0","id":)" + std::to_string(static_cast<std::uint64_t>(request.id.asNumber())) + suffix;
        CHECK(host.plane().connection().answerCall(calls.front().callIndex, answer) == Protocol::Sync::CallAnswer::Sent);
        pumpData(factory.last(), dispatcher, host.publish());
        CHECK(!core.didClaimClipboardServe());
        CHECK(observer.m_clipboardRoutingChanges == 0);
    }
}

/// A version refusal naming the host's wire version and platform.
///
/// - Parameter hostWireVersion: The wire version the refusing host speaks.
/// - Returns: The frame.
std::vector<std::uint8_t> versionRefusalFrame(std::uint16_t hostWireVersion)
{
    Protocol::Sync::Bye refusal { .reason = Protocol::Sync::ByeReason::VersionMismatch,
        .peer = Protocol::Sync::PeerMetadata {
            .build = { .isKnown = true, .wireVersion = hostWireVersion, .appVersion = "0.2.0.7" },
            .platform = "Linux aarch64" } };
    std::vector<std::uint8_t> body, out;
    Protocol::Sync::encode(body, refusal);
    CHECK(Protocol::Sync::appendFrame(out, Protocol::Sync::FrameKind::Bye, 0, body));
    return out;
}

// The last refusal is a fact a frontend reads after the connection fails:
// which reason, and for a version refusal which side is older and what the
// host runs.
TEST(aVersionRefusalRecordsWhichSideIsOlder)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    check(!core.lastRefusal().hasRefusal, "nothing refused before the handshake");
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    // The wire has one version, so a host that refuses it speaks another.
    const std::uint16_t newer = Ubiquitty::Protocol::Sync::wireVersion + 1;
    pumpData(factory.last(), dispatcher, versionRefusalFrame(newer));
    check(core.state() == State::Failed, "a version refusal is terminal");
    const HostRefusal& refusal = core.lastRefusal();
    check(refusal.hasRefusal, "the refusal was recorded");
    check(refusal.reason == Protocol::Sync::ByeReason::VersionMismatch, "with its reason");
    check(refusal.olderSide == Protocol::Sync::OlderSide::Client, "a host past this client leaves this client the older side");
    check(refusal.hostPlatform == "Linux aarch64", "and the platform the host named");
    check(refusal.hostBuild.isKnown && refusal.hostBuild.appVersion == "0.2.0.7", "and the build the host named");
    check(refusal.detail.find("is from an older build of Ubiquitty") != std::string::npos && refusal.detail.find("(0.2.0.7).") != std::string::npos,
        "and a notice naming this client the older end, with the host's build");
    core.stop();
}

// A host that closes the Sync connection on a refusal has said why it will
// not serve this client. Its typed reason is recorded, never a decode fault that the
// client answers by connecting again to be refused again.
TEST(aByeThatRefusesIsARefusalAndNotAFaultToRetry)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    const auto byeFrame = [](Protocol::Sync::ByeReason reason) {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = reason, .detail = {} });
        std::vector<std::uint8_t> stream;
        check(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Bye, 0, body), "premise: a Bye is a frame");
        return stream;
    };
    pumpData(factory.last(), dispatcher, byeFrame(Protocol::Sync::ByeReason::VersionMismatch));
    check(core.state() == State::Failed, "a Bye for the version is terminal, as the refusal frame for it is");
    check(core.lastRefusal().hasRefusal && core.lastRefusal().reason == Protocol::Sync::ByeReason::VersionMismatch, "and is recorded with its reason");
    core.stop();

    // A Bye that is not a refusal is a connection that ended: the client
    // connects again.
    RemoteSessionCore retried(term.get(), &dispatcher, &factory, &observer);
    retried.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, helloAckFrame(7));
    pumpData(factory.last(), dispatcher, byeFrame(Protocol::Sync::ByeReason::Superseded));
    check(retried.state() != State::Failed && !retried.lastRefusal().hasRefusal, "a Bye that refuses nothing is not recorded as a refusal");
    retried.stop();
}

// A typed version refusal may omit peer metadata. Neither side nor a peer
// build may be invented, and deployment still requires an explicit action.
TEST(anUnreadableRefusalIsAVersionRefusalWithNeitherSideNamed)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    pumpData(factory.last(), dispatcher, authDeniedFrame(Protocol::Sync::ByeReason::VersionMismatch));
    check(core.state() == State::Failed, "a typed version refusal ends the attach");
    const HostRefusal& refusal = core.lastRefusal();
    check(refusal.hasRefusal, "and is recorded as a refusal");
    check(refusal.reason == Protocol::Sync::ByeReason::VersionMismatch, "a protocol version refusal, not the byte it led with");
    check(refusal.olderSide == Protocol::Sync::OlderSide::Unknown, "the ranges name no side");
    check(!refusal.hostBuild.isKnown, "the host stated no build");
    check(refusal.detail.find("doesn’t say which build it runs") != std::string::npos, "told the host predates the range handshake");
    check(refusal.detail.find("whichever") == std::string::npos, "never told to update whichever is older");
    check(observer.m_deploys == 0, "and nothing is pushed until someone asks for the update");
    core.stop();
}

TEST(aTruncatedTypedByeCannotInventPeerMetadataOrARefusal)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    const std::array<std::uint8_t, 1> body { std::to_underlying(Protocol::Sync::ByeReason::TokenMissing) };
    std::vector<std::uint8_t> frame;
    REQUIRE(Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Bye, 0, body));
    pumpData(factory.last(), dispatcher, frame);
    CHECK(core.state() == State::Reconnecting);
    CHECK(factory.last() == nullptr);
    CHECK(factory.createCount() == 1);
    CHECK(!core.acknowledged());
    CHECK(!core.lastRefusal().hasRefusal);
    CHECK(!core.lastRefusal().hostBuild.isKnown);
    CHECK(core.lastRefusal().hostPlatform.empty());
    CHECK(observer.m_deploys == 0);
    core.stop();
}

// An update installs before anything runs on the host: the core dials
// nothing until the install says how it ended. An install that failed leaves
// the core Failed with the install's own reason, and no carrier is ever
// started, so no bootstrap runs the old host's daemon start. One that worked
// dials the freshly installed host.
TEST(anUpdateInstallsBeforeAnythingRunsOnTheHost)
{
    {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.updateHostBeforeConnect("Darwin arm64");
        core.start(0, 80, 24);
        check(core.state() == State::Deploying, "an update-first start goes straight to the install");
        check(factory.createCount() == 0, "and dials nothing");
        dispatcher.pump();
        check(observer.m_deploys == 1, "the install is requested once");
        check(observer.m_deployPlatform == "Darwin arm64", "with the platform the caller named");
        check(factory.createCount() == 0, "and still nothing is dialled while it runs");
        core.deployFinished(DeployResult::Failed, "the upload arrived damaged");
        check(core.state() == State::Failed, "a failed install fails the window");
        check(!observer.m_failureReasons.empty() && observer.m_failureReasons.back() == "the upload arrived damaged",
            "with the install's own reason");
        check(factory.createCount() == 0, "and nothing ever ran on the host");
        check(!dispatcher.hasDelayed(), "nor will: no reconnect is scheduled");
        core.stop();
    }
    {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.updateHostBeforeConnect("");
        core.start(0, 80, 24);
        dispatcher.pump();
        check(observer.m_deploys == 1, "a platform the caller could not name is still updated");
        core.deployFinished(DeployResult::Succeeded, "");
        check(factory.createCount() == 1, "a finished install dials the new host");
        pumpConnect(factory.last(), dispatcher);
        pumpData(factory.last(), dispatcher, helloAckFrame(5));
        check(core.state() == State::Connected, "and attaches to it");
        core.stop();
    }
    {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.start(0, 80, 24);
        core.updateHostBeforeConnect("Darwin arm64");
        dispatcher.pump();
        check(observer.m_deploys == 0, "a core already dialling is not turned into an update");
        core.stop();
    }
}

// The carrier is a fact: which one is up, and why QUIC is not it when it was
// asked for. The reason is the factory's own words.
TEST(theCarrierFactSaysWhyQUICFellBack)
{
    auto term = makeTerm();
    FakeDispatcher dispatcher;
    FakeFactory factory;
    factory.setQUICFallbackReason("the host didn’t say how to connect to it over QUIC");
    RecordingObserver observer;
    RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    check(core.carrierFact().carrier == Carrier::None, "no carrier before one is up");
    core.start(0, 80, 24);
    pumpConnect(factory.last(), dispatcher);
    const CarrierFact fact = core.carrierFact();
    check(fact.carrier == Carrier::Stream, "a stream carrier is up");
    check(fact.quicFallbackReason == "the host didn’t say how to connect to it over QUIC", "with the factory's reason QUIC fell back");
    core.stop();
}

// A token set on the connection is the Greet's opaque credential. With none,
// the mandatory credential-presence field is false and no credential follows.
TEST(aConnectionPresentsItsTokenOnTheHandshake)
{
    for (const std::string& token : { std::string("secret-token"), std::string() }) {
        auto term = makeTerm();
        FakeDispatcher dispatcher;
        FakeFactory factory;
        RecordingObserver observer;
        RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
        core.setAuthToken(token);
        core.start(0, 80, 24);
        pumpConnect(factory.last(), dispatcher);
        const auto hello = factory.firstOfType(helloType);
        check(hello.has_value(), "a Greet was sent");
        if (!hello)
            continue;
        const auto decoded = Protocol::Sync::decodeGreet(*hello);
        check(decoded.has_value(), "and it decodes");
        if (!decoded)
            continue;
        check(decoded->credential == (token.empty() ? std::nullopt : std::optional { std::vector<std::uint8_t>(token.begin(), token.end()) }), "and the token rides the Greet");
        core.stop();
    }
}

UBIQUITTY_TEST_MAIN()
