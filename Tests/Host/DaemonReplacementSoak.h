// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/HostPaths.h"

// Included beside the acceptance fixture's dispatcher and deterministic peer.

namespace {

/// Observes outgoing receipts without replacing the production carrier.
class ReplacementChannel final : public Transport::Channel {
public:
    ReplacementChannel(std::unique_ptr<Transport::Channel> channel, std::uint64_t& receipt)
        : m_channel(std::move(channel))
        , m_receipt(receipt)
    {
    }
    /// Starts the wrapped carrier with the session's observer.
    ///
    /// - Parameters:
    ///   - observer: Observer retained by the carrier until it stops.
    /// - Returns: Whether the carrier accepted startup.
    bool start(Transport::ChannelObserver* observer) override { return m_channel->start(observer); }
    /// Stops the wrapped carrier and its callbacks.
    void stop() override { m_channel->stop(); }
    /// Checks receipt continuity before forwarding stream bytes.
    ///
    /// - Parameters:
    ///   - bytes: Encoded frames borrowed for this call.
    void send(std::span<const std::uint8_t> bytes) override
    {
        m_decoder.append(bytes);
        while (const auto frame = m_decoder.next()) {
            if (frame->kind == Sync::FrameKind::Ack) {
                if (const auto ack = Sync::decodeAck(frame->body)) {
                    for (const auto& pane : ack->panes) {
                        CHECK(pane.effectsAcknowledged >= m_receipt);
                        m_receipt = pane.effectsAcknowledged;
                    }
                }
            } else if (frame->kind == Sync::FrameKind::Greet) {
                if (const auto greet = Sync::decodeGreet(frame->body); greet && greet->resume) {
                    REQUIRE(greet->resume->panes.size() == 1);
                    CHECK_EQ(greet->resume->panes.front().effectsAcknowledged, m_receipt);
                }
            }
        }
        m_channel->send(bytes);
    }
    /// Preserves the carrier's datagram admission decision.
    ///
    /// - Parameters:
    ///   - bytes: Datagram borrowed for this call.
    /// - Returns: Whether the carrier accepted the datagram.
    bool sendDatagram(std::span<const std::uint8_t> bytes) override { return m_channel->sendDatagram(bytes); }
    /// Exposes the authenticated carrier identity.
    ///
    /// - Returns: The verified certificate pin, when available.
    std::optional<std::array<std::uint8_t, 32>> authenticatedCertificatePin() const override { return m_channel->authenticatedCertificatePin(); }
    /// Preserves authentication failures from the carrier.
    ///
    /// - Returns: Whether peer authentication failed.
    bool peerAuthenticationFailed() const override { return m_channel->peerAuthenticationFailed(); }
    /// Preserves the carrier's setup retry policy.
    ///
    /// - Returns: Whether initial setup failures are bounded by the carrier.
    bool boundsInitialSetupFailures() const override { return m_channel->boundsInitialSetupFailures(); }
    /// Exposes the peer's application close reason.
    ///
    /// - Returns: The close code, when supplied by the peer.
    std::optional<std::uint64_t> peerApplicationCloseCode() const override { return m_channel->peerApplicationCloseCode(); }
    /// Suspends carrier reads under session backpressure.
    void suspendReading() override { m_channel->suspendReading(); }
    /// Resumes reads suspended by the session.
    void resumeReading() override { m_channel->resumeReading(); }
    /// Exposes outbound pressure without adding another queue.
    ///
    /// - Returns: The carrier's queued byte count.
    std::int64_t pendingOutboundBytes() const override { return m_channel->pendingOutboundBytes(); }
    /// Exposes the carrier's outbound admission bound.
    ///
    /// - Returns: The carrier's outbound capacity in bytes.
    std::int64_t outboundCapacityBytes() const override { return m_channel->outboundCapacityBytes(); }
    /// Preserves the carrier's trust origin.
    ///
    /// - Returns: The wrapped carrier's origin.
    Transport::Origin origin() const override { return m_channel->origin(); }
    /// Exposes carrier health to the session owner.
    ///
    /// - Returns: The wrapped carrier's health.
    Transport::Health health() const override { return m_channel->health(); }
    /// Preserves the carrier's diagnostic description.
    ///
    /// - Returns: The wrapped carrier's description.
    std::string description() const override { return m_channel->description(); }
    /// Reads statistics from the production carrier.
    ///
    /// - Parameters:
    ///   - stats: Destination for a supported statistics query.
    /// - Returns: Whether the carrier supports the query.
    bool queryStats(Transport::TransportStats& stats) const override { return m_channel->queryStats(stats); }
    /// Delegates path migration to the production carrier.
    void repath() override { m_channel->repath(); }

private:
    std::unique_ptr<Transport::Channel> m_channel;
    Sync::FrameDecoder m_decoder;
    std::uint64_t& m_receipt;
};

/// Permits one bootstrap and requires production resume proofs thereafter.
class ReplacementFactory final : public Session::ChannelFactory {
public:
    explicit ReplacementFactory(Sync::QUICBootstrapInfo bootstrap)
        : m_bootstrap(std::move(bootstrap))
    {
    }
    /// Consumes the bootstrap token at most once.
    ///
    /// - Parameters:
    ///   - done: Receives the carrier, or failure after a repeated bootstrap.
    void createChannel(Session::TransportMode, Done done) override
    {
        ++creations;
        if (creations != 1) {
            done(nullptr, true);
            return;
        }
        deliver(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_bootstrap.port, std::move(m_bootstrap.token),
                    m_bootstrap.certificateSHA256, Transport::Origin { Transport::OriginKind::Daemon, "" }),
            std::move(done));
    }
    /// Advertises the inherited QUIC listener's resume support.
    ///
    /// - Returns: True because each successor inherits the listener.
    bool canResumeChannel() const override { return true; }
    /// Records the daemon identity reported by the session owner.
    ///
    /// - Parameters:
    ///   - leaf: Host socket basename received through handover.
    /// - Returns: Whether the basename has the host socket format.
    bool pinDaemonSocket(std::string_view leaf) override
    {
        socket = leaf;
        return Ubiquitty::Host::isHostSocketLeaf(leaf);
    }
    /// Reconnects with the session owner's exported resume proof.
    ///
    /// - Parameters:
    ///   - key: Proof builder bound to the resumed session.
    ///   - done: Receives the receipt-observing carrier.
    void resumeChannel(Session::ResumeKey key, Done done) override
    {
        ++resumes;
        Transport::ExportedPreamble proof { .exporterLabel = std::move(key.exporterLabel), .build = std::move(key.buildPreamble) };
        deliver(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_bootstrap.port, std::move(proof),
                    m_bootstrap.certificateSHA256, Transport::Origin { Transport::OriginKind::Daemon, "" }),
            std::move(done));
    }
    unsigned creations = 0;
    unsigned resumes = 0;
    std::uint64_t receipt = 0;
    std::string socket;

private:
    /// Wraps a carrier without replacing its transport behavior.
    ///
    /// - Parameters:
    ///   - channel: Production carrier transferred to the session.
    ///   - done: Completion receiving the wrapped carrier.
    void deliver(std::unique_ptr<Transport::Channel> channel, Done done)
    {
        done(std::make_unique<ReplacementChannel>(std::move(channel), receipt), true);
    }
    Sync::QUICBootstrapInfo m_bootstrap;
};

/// Checks live handover overlap and exactly ordered command effects.
class ReplacementObserver final : public Session::RemoteSessionObserver {
public:
    /// Rejects duplicate or reordered command completion effects.
    ///
    /// - Parameters:
    ///   - effect: Published effect carrying its monotonic sequence.
    void onCommandDone(const Sync::EffectView& effect, const Sync::CommandFinishedEffect&, Sync::EffectDisposition) override
    {
        CHECK(effect.sequence > lastEffect);
        lastEffect = effect.sequence;
        ++completions;
    }
    /// Requires each handover to begin while its flood is still running.
    ///
    /// - Parameters:
    ///   - state: Session state published by the production owner.
    ///   - detail: Failure description distinguishing fixture shutdown.
    void onStateChanged(Session::State state, const std::string& detail) override
    {
        if (state == Session::State::Reconnecting && iteration > liveHandovers) {
            CHECK_MSG(completions < iteration, "the flood ended before the handover began");
            ++liveHandovers;
        }
        if (state == Session::State::Failed && detail != "detached") {
            std::fprintf(stderr, "replacement-soak: failed: %s\n", detail.c_str());
            ++failures;
        }
    }
    std::uint64_t lastEffect = 0;
    unsigned completions = 0;
    unsigned iteration = 0;
    unsigned liveHandovers = 0;
};

/// Owns one production session and its independent verification state.
struct ReplacementClient {
    Ubiquitty::Terminal terminal;
    Ubiquitty::Terminal expected;
    ReplacementFactory factory;
    ReplacementObserver observer;
    Session::RemoteSessionCore core;
    Sync::Welcome welcome;
    Sync::ID128 pane;
    std::vector<std::uint64_t> historyHashes;

    ReplacementClient(Dispatcher& dispatcher, Sync::QUICBootstrapInfo bootstrap)
        : factory(std::move(bootstrap))
        , core(&terminal, &dispatcher, &factory, &observer)
    {
        Ubiquitty::resize(terminal, 24, 80);
        Ubiquitty::resize(expected, 24, 80);
    }
    /// Requires both carrier connection and acknowledged Sync admission.
    ///
    /// - Returns: Whether the client can exchange admitted session traffic.
    bool isConnected() const { return core.state() == Session::State::Connected && core.acknowledged(); }
};

/// Checks two production session owners across daemon replacements without test-driven redials.
///
/// - Parameters:
///   - bootstraps: Distinct initial credentials for the two clients.
///   - iterations: Number of daemon successors driven by the supervisor.
///   - seed: Reproducible generator seed for flood contents and replacement delays.
void runDaemonReplacementSoak(const std::array<Sync::QUICBootstrapInfo, 2>& bootstraps, unsigned iterations, std::uint32_t seed)
{
    Dispatcher dispatcher;
    std::array<std::unique_ptr<ReplacementClient>, 2> clients;
    for (unsigned index = 0; index < clients.size(); ++index) {
        clients[index] = std::make_unique<ReplacementClient>(dispatcher, bootstraps[index]);
        auto& client = *clients[index];
        client.core.setClientInstance({ .low = index + 1, .high = seed + 1 });
        client.core.setWorkspace("replacement-" + std::to_string(index), false);
        client.core.start(0, 80, 24);
        REQUIRE(dispatcher.until([&] { return client.isConnected() && screenText(client.terminal).contains("PEER_READY"); }));
        const auto* connection = Session::RemoteSessionDatagramIngressAccess::screen(client.core);
        REQUIRE(connection && connection->welcome() && connection->boundPanes().size() == 1);
        client.welcome = *connection->welcome();
        client.pane = connection->boundPanes().front().second;
        client.core.sendInput("HISTORY replacement-" + std::to_string(index) + "\n");
        REQUIRE(dispatcher.until([&] { return screenText(client.terminal).contains("HISTORY_END") && client.core.view().history && client.core.view().history->head() > 40; }));
        const Sync::LineRange range { .from = 1, .to = client.core.view().history->head() };
        REQUIRE(client.core.wantHistory(Sync::HistoryPriority::Viewport, { &range, 1 }));
        REQUIRE(dispatcher.until([&] {
            const auto* history = client.core.view().history;
            for (auto line = range.from; line < range.to; ++line) {
                if (!history || !history->line(line))
                    return false;
            }
            return true;
        }));
        for (auto line = range.from; line < range.to; ++line)
            client.historyHashes.push_back(client.core.view().history->line(line)->contentHash());
    }
    CHECK(clients[0]->pane != clients[1]->pane);
    std::mt19937 random(seed);
    for (unsigned iteration = 1; iteration <= iterations; ++iteration) {
        const auto iterationSeed = static_cast<std::uint32_t>(random());
        const unsigned delayMilliseconds = random() % 40;
        for (unsigned index = 0; index < clients.size(); ++index) {
            auto& client = *clients[index];
            const auto floodSeed = iterationSeed ^ index;
            client.observer.iteration = iteration;
            Ubiquitty::processBytes(&client.expected, UbiquittyTest::soakCommandStart(floodSeed));
            for (unsigned frame = 0; frame < 256; ++frame)
                Ubiquitty::processBytes(&client.expected, UbiquittyTest::soakFrame(floodSeed, frame));
            Ubiquitty::processBytes(&client.expected, "\033]133;D;0\a");
            client.core.sendInput("REPLACE " + std::to_string(floodSeed) + "\n");
        }
        (void)dispatcher.until([] { return false; }, static_cast<int>(delayMilliseconds));
        std::printf("replacement-soak: replace iteration=%u seed=%u delay_ms=%u\n", iteration, iterationSeed, delayMilliseconds);
        std::fflush(stdout);
        std::string successor;
        const auto began = std::chrono::steady_clock::now();
        REQUIRE(dispatcher.until([&] {
            pollfd input { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
            if (::poll(&input, 1, 0) <= 0)
                return false;
            char character = 0;
            if (::read(STDIN_FILENO, &character, 1) != 1)
                return false;
            if (character == '\n')
                return true;
            successor += character;
            return successor.size() > 128;
        },
            30'000));
        REQUIRE(Ubiquitty::Host::isHostSocketLeaf(successor));
        for (auto& owned : clients) {
            auto& client = *owned;
            REQUIRE_MSG(dispatcher.until([&] {
                const auto* connection = Session::RemoteSessionDatagramIngressAccess::screen(client.core);
                return client.isConnected() && connection && connection->welcome()
                    && connection->welcome()->hostInstanceID != client.welcome.hostInstanceID
                    && UbiquittyTest::isSoakScreenEqual(&client.terminal, client.expected)
                    && client.observer.completions == iteration && client.factory.receipt >= client.observer.lastEffect;
            },
                            15'000),
                "a client did not resume on the successor with the exact flood and completion receipt");
            const auto* connection = Session::RemoteSessionDatagramIngressAccess::screen(client.core);
            REQUIRE(connection && connection->welcome());
            const auto next = *connection->welcome();
            CHECK(next.predecessorHostInstanceID == client.welcome.hostInstanceID);
            CHECK(next.attachmentID == client.welcome.attachmentID);
            CHECK_EQ(next.selection.sessionID, client.welcome.selection.sessionID);
            CHECK(next.selection.socketName == successor);
            CHECK(client.factory.socket == successor);
            CHECK(connection->boundPanes().size() == 1 && connection->boundPanes().front().second == client.pane);
            CHECK_EQ(client.factory.creations, 1u);
            CHECK_EQ(client.observer.liveHandovers, iteration);
            CHECK(client.factory.resumes >= iteration && client.factory.resumes <= iteration * 3);
            CHECK_EQ(connection->counters().digestMismatches, 0u);
            const Sync::LineRange range { .from = 1, .to = client.historyHashes.size() + 1 };
            // Evict the passive cache so retained client pages cannot satisfy
            // the assertion without the successor serving its own history.
            const auto* history = client.core.view().history;
            REQUIRE(history);
            const_cast<Sync::HistoryCache*>(history)->trimToBytes(0);
            REQUIRE(!history->line(range.from));
            REQUIRE(client.core.wantHistory(Sync::HistoryPriority::Viewport, { &range, 1 }));
            REQUIRE(dispatcher.until([&] {
                const auto* history = client.core.view().history;
                for (std::size_t index = 0; index < client.historyHashes.size(); ++index) {
                    if (!history || !history->line(index + 1) || history->line(index + 1)->contentHash() != client.historyHashes[index])
                        return false;
                }
                return true;
            }));
            client.welcome = next;
        }
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        std::printf("replacement-soak: complete iteration=%u seed=%u resume_ms=%lld receipts=%llu,%llu\n", iteration, iterationSeed,
            static_cast<long long>(milliseconds), static_cast<unsigned long long>(clients[0]->factory.receipt), static_cast<unsigned long long>(clients[1]->factory.receipt));
        std::fflush(stdout);
        REQUIRE(!failures);
    }
    for (auto& client : clients)
        client->core.stop();
    dispatcher.pump();
    std::printf("replacement-soak: ok iterations=%u seed=%u\n", iterations, seed);
}

} // namespace
