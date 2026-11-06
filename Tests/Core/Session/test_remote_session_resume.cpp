// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_remote_session_resume — a remote session continues one attachment
// across its carriers (Docs/sync.md §6.9): every reconnect is a resume, a
// QUIC redial presents a resume proof and runs no bootstrap, a proof the host
// refuses bootstraps over QUIC and an unanswered path bootstraps over SSH, a carrier the
// attachment declared dead is redialled at once, a suspension tells the host
// and waits, and the client is one instance to the host for as long as its
// frontend keeps the instance.
//
// The core is driven over stub carriers against the host's real attachment
// table and connection, with one clock for both ends and the dispatcher.

#include "Core/Protocol/Sync/AttachmentLink.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Liveness.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Session/ReconnectBackoff.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Session;
using namespace Ubiquitty::Protocol::Sync;
namespace Link = UbiquittyTest::AttachmentLink;

namespace {

constexpr std::uint64_t second = 1'000'000;

/// The session and daemon instance whose pane is `Link::paneID`: the host
/// mints a pane's identity from the two.
constexpr std::uint32_t sessionID = 9;
constexpr std::uint64_t daemonInstanceID = 10;

/// Runs posted work in order and delayed work when the clock reaches it, and
/// is the clock both ends read.
class ClockedDispatcher final : public Dispatcher {
public:
    /// Queues work for the next `pump`.
    ///
    /// - Parameter work: The work.
    void post(PostedWork work) override { m_queue.push_back(std::move(work)); }

    /// Queues work for when the clock has moved on by a delay.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: The work.
    void postDelayed(double seconds, PostedWork work) override
    {
        m_delaysRequested.push_back(seconds);
        m_delayed.push_back({ .due = m_now + static_cast<std::uint64_t>(seconds * static_cast<double>(second)), .work = std::move(work) });
    }

    /// Runs the posted work, and what it posts.
    void pump()
    {
        while (!m_queue.empty()) {
            PostedWork work = std::move(m_queue.front());
            m_queue.erase(m_queue.begin());
            std::move(work)();
        }
    }

    /// Moves the clock on, running each delayed piece of work as its time
    /// comes, the earliest first.
    ///
    /// - Parameter microseconds: How far.
    void advance(std::uint64_t microseconds)
    {
        const std::uint64_t until = m_now + microseconds;
        pump();
        for (;;) {
            const auto next = std::ranges::min_element(m_delayed, {}, &Delayed::due);
            if (next == m_delayed.end() || next->due > until)
                break;
            m_now = std::max(m_now, next->due);
            PostedWork work = std::move(next->work);
            m_delayed.erase(next);
            std::move(work)();
            pump();
        }
        m_now = until;
    }

    /// The clock, in microseconds.
    [[nodiscard]] std::uint64_t now() const { return m_now; }

    /// Every delay asked for, in order.
    [[nodiscard]] const std::vector<double>& delaysRequested() const { return m_delaysRequested; }

private:
    /// Work waiting for its time.
    struct Delayed {
        std::uint64_t due = 0;
        PostedWork work;
    };

    std::vector<PostedWork> m_queue;
    std::vector<Delayed> m_delayed;
    std::vector<double> m_delaysRequested;
    std::uint64_t m_now = 100 * second;
};

using FrameLog = std::vector<std::vector<std::uint8_t>>;

/// What a bootstrap asked for QUIC builds.
enum class Bootstrap : std::uint8_t {
    /// A stream carrier, whatever was asked for.
    Stream,
    /// A QUIC carrier.
    QUIC,
};

/// A carrier that records what the core writes and lets a test speak as its
/// I/O thread.
class StubChannel final : public Transport::Channel {
public:
    /// - Parameters:
    ///   - sent: Where the core's writes are recorded; it outlives the
    ///     channel.
    ///   - live: The factory's pointer to the channel, cleared when the core
    ///     drops it.
    StubChannel(FrameLog* sent, StubChannel** live)
        : m_sent(sent)
        , m_live(live)
    {
    }

    ~StubChannel() override
    {
        if (*m_live == this)
            *m_live = nullptr;
    }

    /// Keeps the observer; the carrier connects when the test says so.
    ///
    /// - Parameter observer: The core.
    /// - Returns: True: the carrier started.
    bool start(Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }

    /// Records one write.
    ///
    /// - Parameter data: The bytes.
    void send(std::span<const std::uint8_t> data) override { m_sent->emplace_back(data.begin(), data.end()); }

    /// Counts a datagram the core offered, and refuses it: the core sends it
    /// on the stream instead.
    ///
    /// - Returns: False.
    [[nodiscard]] bool sendDatagram(std::span<const std::uint8_t>) override
    {
        ++m_datagramsOffered;
        return false;
    }

    /// How many datagrams the core offered this carrier.
    [[nodiscard]] std::size_t datagramsOffered() const { return m_datagramsOffered; }

    void stop() override { }
    [[nodiscard]] Transport::Origin origin() const override { return { Transport::OriginKind::Daemon, "" }; }
    [[nodiscard]] Transport::Health health() const override { return Transport::Health::Connected; }
    [[nodiscard]] std::string description() const override { return "stub"; }

    /// Reports the carrier connected.
    void connect() { m_observer->onConnect(); }

    /// Hands the core bytes from the host.
    ///
    /// - Parameter bytes: The bytes.
    void deliver(const std::vector<std::uint8_t>& bytes) { m_observer->onData(bytes); }

    /// Reports the carrier closed.
    ///
    /// - Parameter reason: Why.
    void close(const std::string& reason) { m_observer->onClose(reason); }

    /// Reports a QUIC application close carrying the host's generic proof
    /// refusal, independently of the stream's diagnostic text.
    void refuseProof()
    {
        m_applicationCloseCode = quicCloseCodeOf(ByeReason::Malformed);
        close("the host refused the proof");
    }

    [[nodiscard]] std::optional<std::uint64_t> peerApplicationCloseCode() const override { return m_applicationCloseCode; }

private:
    FrameLog* m_sent;
    StubChannel** m_live;
    Transport::ChannelObserver* m_observer = nullptr;
    std::size_t m_datagramsOffered = 0;
    std::optional<std::uint64_t> m_applicationCloseCode;
};

/// Builds stub carriers and counts how each was asked for: through the
/// bootstrap, which is where a real factory runs ssh, or by a resume key,
/// where it runs nothing. Once a bootstrap has built a QUIC carrier the
/// factory can dial it again, as one that kept the address, port and pin can.
class StubFactory final : public ChannelFactory {
public:
    enum class ResumeDelivery : std::uint8_t {
        Channel,
        Unavailable,
        Pending
    };

    /// - Parameter bootstrap: What a bootstrap asked for QUIC builds.
    explicit StubFactory(Bootstrap bootstrap)
        : m_bootstrap(bootstrap)
    {
    }

    /// Sets what the next bootstrap asked for QUIC builds.
    ///
    /// - Parameter bootstrap: The carrier.
    void setBootstrap(Bootstrap bootstrap) { m_bootstrap = bootstrap; }

    /// Chooses how a proof dial's factory completion is delivered.
    void setResumeDelivery(ResumeDelivery delivery) { m_resumeDelivery = delivery; }

    /// Completes a held proof request without a carrier.
    void finishPendingResume() { std::exchange(m_pendingResume, {})(nullptr, false); }

    /// Builds a carrier through the bootstrap.
    ///
    /// - Parameters:
    ///   - transport: The carrier asked for.
    ///   - done: Receives the carrier.
    void createChannel(TransportMode transport, Done done) override
    {
        ++m_bootstraps;
        const Bootstrap built = transport == TransportMode::QUIC ? m_bootstrap : Bootstrap::Stream;
        m_hasReach = m_hasReach || built == Bootstrap::QUIC;
        build(std::move(done), built);
    }

    /// Whether a bootstrap has built a QUIC carrier this factory can dial
    /// again.
    [[nodiscard]] bool canResumeChannel() const override { return m_hasReach; }

    /// Builds a QUIC carrier that presents a resume key.
    ///
    /// - Parameters:
    ///   - key: The key, kept for the host to read its preamble.
    ///   - done: Receives the carrier.
    void resumeChannel(ResumeKey key, Done done) override
    {
        ++m_resumes;
        m_key = std::move(key);
        if (m_resumeDelivery == ResumeDelivery::Unavailable) {
            done(nullptr, false);
            return;
        }
        if (m_resumeDelivery == ResumeDelivery::Pending) {
            m_pendingResume = std::move(done);
            return;
        }
        build(std::move(done), Bootstrap::QUIC);
    }

    /// How many carriers were asked for through the bootstrap.
    [[nodiscard]] std::size_t bootstrapCount() const { return m_bootstraps; }

    /// How many carriers were asked for by a resume key.
    [[nodiscard]] std::size_t resumeCount() const { return m_resumes; }

    /// The key the last resumed carrier presents.
    [[nodiscard]] const ResumeKey& key() const { return m_key; }

    /// The carrier the core holds, or null.
    [[nodiscard]] StubChannel* last() const { return m_last; }

    /// Every write of every carrier, in order.
    [[nodiscard]] const FrameLog& sent() const { return m_sent; }

private:
    /// Hands the core a new carrier.
    ///
    /// - Parameters:
    ///   - done: Receives the carrier.
    ///   - carrier: Which carrier it is.
    void build(Done done, Bootstrap carrier)
    {
        auto channel = std::make_unique<StubChannel>(&m_sent, &m_last);
        m_last = channel.get();
        done(std::move(channel), carrier == Bootstrap::QUIC);
    }

    Bootstrap m_bootstrap;
    bool m_hasReach = false;
    std::size_t m_bootstraps = 0;
    std::size_t m_resumes = 0;
    ResumeKey m_key;
    ResumeDelivery m_resumeDelivery = ResumeDelivery::Channel;
    Done m_pendingResume;
    StubChannel* m_last = nullptr;
    FrameLog m_sent;
};

/// Counts what the core tells its frontend.
class Observer final : public RemoteSessionObserver {
public:
    /// Records the state.
    ///
    /// - Parameters:
    ///   - state: The state.
    ///   - detail: Why.
    void onStateChanged(State state, const std::string& detail) override
    {
        m_state = state;
        m_detail = detail;
    }

    /// Records the link's indicators.
    ///
    /// - Parameter link: The link.
    void onLinkState(const Protocol::Sync::LinkState& link) override { m_links.push_back(link); }

    /// Records what the pane's input came to.
    ///
    /// - Parameter status: The status.
    void onInDoubt(const PaneInputStatus& status) override { m_inputs.push_back(status); }

    State m_state = State::Idle;
    std::string m_detail;
    std::vector<Protocol::Sync::LinkState> m_links;
    std::vector<PaneInputStatus> m_inputs;
};

/// The daemon's end: an attachment table and a pane that outlive every
/// carrier, and the connection that serves the carrier in hand.
class StubDaemon {
public:
    Link::Host host { { 41, 42 } };

    /// Accepts the carrier the core dialled through the bootstrap: the login
    /// authenticated it.
    ///
    /// - Parameters:
    ///   - factory: The core's carriers.
    ///   - now: The clock.
    void acceptBootstrapped(const StubFactory& factory, std::uint64_t now)
    {
        host.lose(now);
        host.accept();
        begin(factory);
    }

    /// Reads the preamble the carrier presents, as the daemon does once the
    /// handshake has exported its keying material, and accepts the carrier if
    /// the proof is admitted.
    ///
    /// - Parameters:
    ///   - factory: The core's carriers.
    ///   - now: The clock.
    /// - Returns: Whether the proof was admitted. A carrier that is not is
    ///   dropped without a word.
    [[nodiscard]] bool acceptProof(const StubFactory& factory, std::uint64_t now)
    {
        std::array<std::uint8_t, 32> exporter {};
        exporter.fill(++m_exporterSeed);
        CHECK_STR_EQ(factory.key().exporterLabel, std::string(resumeExporterLabel));
        const std::vector<std::uint8_t> preamble = factory.key().buildPreamble(exporter);
        PreambleReader reader;
        (void)reader.feed(preamble);
        const std::optional<Preamble> read = reader.take();
        const ResumeProof* proof = read ? std::get_if<ResumeProof>(&*read) : nullptr;
        host.lose(now);
        if (!proof || !host.table.admitProof(*proof, exporter, host.connectionNumber + 1, now))
            return false;
        host.accept();
        begin(factory);
        return true;
    }

    /// Reads what the core wrote on the carrier since the last call, and
    /// returns its direct Welcome and the screen plane's envelopes.
    ///
    /// - Parameters:
    ///   - factory: The core's carriers.
    ///   - now: The clock.
    /// - Returns: The bytes for the core.
    [[nodiscard]] std::vector<std::uint8_t> serve(const StubFactory& factory, std::uint64_t now)
    {
        std::vector<std::uint8_t> out;
        for (; m_consumed < factory.sent().size(); ++m_consumed)
            host.connection->receiveStream(factory.sent()[m_consumed], now);
        // A fresh attachment's pane is bound once its client is welcomed; a
        // resume the table continued has bound it already.
        if (!m_isPaneSettled && host.connection->state() == ConnectionState::Attached) {
            m_isPaneSettled = true;
            if (host.connection->boundPanes().empty())
                (void)host.connection->bindPane(*host.pane.source);
        }
        host.connection->service(now);
        std::vector<std::uint8_t> envelope;
        for (;;) {
            envelope.clear();
            if (!host.connection->popStreamFrame(envelope, now))
                break;
            out.insert(out.end(), envelope.begin(), envelope.end());
        }
        return out;
    }

    /// Serves the carrier in hand with the connection the test accepted
    /// itself, as when it changed what the host holds first.
    ///
    /// - Parameter factory: The core's carriers.
    void adopt(const StubFactory& factory) { begin(factory); }

    /// Whether a connection serves a carrier.
    [[nodiscard]] bool isServing() const { return host.connection != nullptr; }

private:
    /// Starts reading a new carrier at what the core writes from now on.
    ///
    /// - Parameter factory: The core's carriers.
    void begin(const StubFactory& factory)
    {
        host.connection->setGreetingAdmission([](const Greet& greet) -> std::expected<HostIdentity, Bye> {
            const auto& request = greet.request;
            if (request.target != AdmissionTarget::Session || request.workspaceID
                || (request.sessionID && request.sessionID != sessionID)
                || (request.expectedDaemonInstanceID && request.expectedDaemonInstanceID != daemonInstanceID))
                return std::unexpected(Bye { .reason = ByeReason::SessionGone });
            return HostIdentity {
                .principal = Link::owner.principal,
                .mayDrive = Link::owner.mayDrive,
                .verifiedKind = Link::owner.verifiedKind,
                .selection = { .target = AdmissionTarget::Session, .sessionID = sessionID, .daemonInstanceID = daemonInstanceID, .socketName = "resume-test.sock", .hostPlatform = "fixture" },
            };
        });
        m_consumed = factory.sent().size();
        m_isPaneSettled = false;
    }

    std::size_t m_consumed = 0;
    bool m_isPaneSettled = false;
    std::uint8_t m_exporterSeed = 0;
};

/// A core, its stub carriers and the daemon it attaches to.
struct Remote {
    std::unique_ptr<Terminal> terminal = std::make_unique<Terminal>();
    ClockedDispatcher dispatcher;
    StubFactory factory;
    Observer observer;
    StubDaemon daemon;
    std::optional<RemoteSessionCore> core;

    /// - Parameter bootstrap: What the bootstrap builds when asked for QUIC.
    explicit Remote(Bootstrap bootstrap)
        : factory(bootstrap)
    {
        makeCore();
    }

    /// Makes the core, as a launched frontend does.
    void makeCore()
    {
        core.emplace(terminal.get(), &dispatcher, &factory, &observer);
        core->setClock([this] { return dispatcher.now(); });
    }

    /// Starts the core and attaches it through the bootstrap.
    void attach()
    {
        core->start(0, 80, 24);
        daemon.acceptBootstrapped(factory, dispatcher.now());
        handshake();
    }

    /// Connects the carrier in hand and lets both ends talk until the
    /// session is attached and quiet.
    void handshake()
    {
        factory.last()->connect();
        dispatcher.pump();
        settle();
    }

    /// Carries what each end has for the other, a millisecond at a time,
    /// for a span.
    ///
    /// - Parameter microseconds: How long.
    void settle(std::uint64_t microseconds = 200'000)
    {
        for (const std::uint64_t until = dispatcher.now() + microseconds; dispatcher.now() < until;) {
            carry();
            dispatcher.advance(1'000);
        }
    }

    /// Carries what the daemon has to say to what the core wrote so far.
    void carry()
    {
        if (!factory.last() || !daemon.isServing())
            return;
        const std::vector<std::uint8_t> down = daemon.serve(factory, dispatcher.now());
        if (down.empty())
            return;
        factory.last()->deliver(down);
        dispatcher.pump();
    }

    /// Lets the clock run with the path dead both ways: nothing the core
    /// writes is read, and nothing reaches it.
    ///
    /// - Parameter microseconds: How long.
    void blackhole(std::uint64_t microseconds)
    {
        for (const std::uint64_t until = dispatcher.now() + microseconds; dispatcher.now() < until;)
            dispatcher.advance(1'000);
    }

    /// The host's program prints, and the host publishes the frame.
    ///
    /// - Parameter text: What it prints.
    void print(std::string_view text)
    {
        UbiquittyTest::feed(daemon.host.pane.terminal, text);
        daemon.host.pane.source->publish(FrameClass::Settled, dispatcher.now());
    }

    /// Where the core's picture and the host's first differ, empty when they
    /// agree.
    [[nodiscard]] std::string disagreement() const { return UbiquittyTest::firstDisagreement(daemon.host.pane.terminal, *terminal); }
};

} // namespace

// The carrier dies at its idle timeout and the session dials again. The
// factory kept the QUIC carrier's reach, and the attachment holds a secret:
// the redial presents a proof, the bootstrap is not run again, and the host
// continues the attachment with a diff into the terminal the frontend draws.
TEST(aQUICRedialPresentsAProofAndRunsNoBootstrap)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    REQUIRE(remote.core->isUsingQUIC());
    remote.print("before the drop\r\n");
    remote.settle();
    REQUIRE(remote.disagreement().empty());
    const ID128 attachment = remote.daemon.host.connection->attachment()->attachmentID;

    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    CHECK(remote.core->state() == State::Reconnecting);
    remote.print("while away\r\n");
    remote.dispatcher.advance(2 * second);

    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
    CHECK_EQ(remote.factory.resumeCount(), 1u);
    REQUIRE(remote.factory.last() != nullptr);
    REQUIRE(remote.daemon.acceptProof(remote.factory, remote.dispatcher.now()));
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.core->isUsingQUIC());
    REQUIRE(remote.daemon.host.connection->attachment() != nullptr);
    CHECK(remote.daemon.host.connection->attachment()->attachmentID == attachment);
    CHECK(!remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_EQ(remote.daemon.host.connection->counters().checkpointsSent, 0u);
    CHECK(remote.disagreement().empty());

    // The proof is the new secret's the next time: a second drop resumes too.
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    remote.dispatcher.advance(2 * second);
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
    CHECK_EQ(remote.factory.resumeCount(), 2u);
    REQUIRE(remote.daemon.acceptProof(remote.factory, remote.dispatcher.now()));
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->attachment()->attachmentID == attachment);
}

// A stream carrier has no way in but the login, and the login authenticated
// it: the reconnect resumes with the secret inside the stream, as the same
// client, and is sent a diff.
TEST(aStreamReconnectResumesTheAttachmentWithADiff)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(!remote.core->isUsingQUIC());
    const ID128 attachment = remote.daemon.host.connection->attachment()->attachmentID;
    CHECK(remote.daemon.host.connection->clientInstance() == remote.core->clientInstance());

    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.print("while away\r\n");
    remote.dispatcher.advance(2 * second);
    CHECK_EQ(remote.factory.bootstrapCount(), 2u);
    CHECK_EQ(remote.factory.resumeCount(), 0u);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();

    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->attachment()->attachmentID == attachment);
    CHECK(!remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_EQ(remote.daemon.host.connection->counters().checkpointsSent, 0u);
    CHECK(remote.daemon.host.connection->clientInstance() == remote.core->clientInstance());
    CHECK(remote.disagreement().empty());
    // What a frontend reads to say the attachment was continued, not redrawn.
    CHECK_EQ(remote.core->resumedPaneCount(ResumeOutcome::Diffed), 1u);
    CHECK_EQ(remote.core->resumedPaneCount(ResumeOutcome::Checkpointed), 0u);
}

// The host freed the attachment while the client was away, so it refuses the
// proof and drops the carrier. The session goes through the bootstrap at
// once, with no backoff, and the carrier the login admitted attaches fresh:
// the picture arrives whole.
TEST(aRefusedProofFallsBackToTheBootstrapAtOnce)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    remote.print("while away\r\n");
    // The host's grace runs out while the client is away.
    remote.daemon.host.lose(remote.dispatcher.now());
    remote.daemon.host.table.expire(remote.dispatcher.now() + measuredResumePolicy().lostGraceMicroseconds + second);
    remote.dispatcher.advance(1'500'000);
    REQUIRE(remote.factory.resumeCount() == 1);
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);

    // The proof dial in hand is refused: its carrier closes.
    const std::size_t resumes = remote.factory.resumeCount();
    REQUIRE(remote.factory.last() != nullptr);
    CHECK(!remote.daemon.acceptProof(remote.factory, remote.dispatcher.now()));
    const std::size_t delaysBefore = remote.dispatcher.delaysRequested().size();
    remote.factory.last()->refuseProof();
    remote.dispatcher.pump();
    CHECK_EQ(remote.factory.bootstrapCount(), 2u);
    CHECK_EQ(remote.factory.resumeCount(), resumes);
    CHECK_EQ(remote.dispatcher.delaysRequested().size(), delaysBefore);

    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.core->isUsingQUIC());
    CHECK(remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_EQ(remote.daemon.host.connection->counters().checkpointsSent, 1u);
    CHECK(remote.disagreement().empty());

    // A host refusal preserves QUIC for a later proof redial.
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    remote.dispatcher.advance(2 * second);
    CHECK_EQ(remote.factory.bootstrapCount(), 2u);
    CHECK_EQ(remote.factory.resumeCount(), resumes + 1);
    CHECK(remote.core->isUsingQUIC());
}

// The path the proof is dialled over carries nothing, and nothing says so.
// Two seconds on, the session is through the SSH bootstrap instead.
TEST(aBlackholedStubPathRedialsWithinTwoSeconds)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    // The reconnect delay, and no more: the proof dial has just begun.
    for (int step = 0; step < 5'000 && remote.factory.resumeCount() == 0; ++step)
        remote.dispatcher.advance(1'000);
    REQUIRE(remote.factory.resumeCount() == 1);
    const std::uint64_t dialledAt = remote.dispatcher.now();
    remote.factory.last()->connect();
    remote.dispatcher.pump();

    std::uint64_t bootstrappedAt = 0;
    for (int step = 0; step < 5'000 && bootstrappedAt == 0; ++step) {
        remote.dispatcher.advance(1'000);
        if (remote.factory.bootstrapCount() == 2)
            bootstrappedAt = remote.dispatcher.now();
    }
    REQUIRE(bootstrappedAt != 0);
    CHECK_MSG(bootstrappedAt - dialledAt <= 2 * second, ("the bootstrap began " + std::to_string(bootstrappedAt - dialledAt) + " us after the proof was dialled").c_str());
    CHECK_EQ(remote.factory.resumeCount(), 1u);
    CHECK(!remote.core->isUsingQUIC());

    // The SSH carrier that reaches its admitted Welcome is left alone.
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    remote.dispatcher.advance(1'500'000);
    REQUIRE(remote.factory.resumeCount() == 1);
    REQUIRE(remote.factory.bootstrapCount() == 3);
    REQUIRE(!remote.core->isUsingQUIC());
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    StubChannel* kept = remote.factory.last();
    remote.settle(3 * second);
    CHECK(remote.factory.last() == kept);
    CHECK_EQ(remote.factory.bootstrapCount(), 3u);
    CHECK(remote.core->state() == State::Connected);
}

TEST(lifecycleChurnCannotKeepRenewingAnUnansweredResumeDial)
{
    for (const bool shouldReportWake : { false, true }) {
        Remote remote(Bootstrap::QUIC);
        remote.attach();
        REQUIRE(remote.core->state() == State::Connected);
        remote.core->suspend();
        remote.core->resumeAfterSuspension(120);
        remote.dispatcher.pump();
        REQUIRE(remote.factory.resumeCount() == 1);

        for (unsigned move = 0; move < 30; ++move) {
            remote.dispatcher.advance(100'000);
            if (shouldReportWake)
                remote.core->resumeAfterSuspension(120);
            else
                remote.core->networkPathChanged();
            remote.dispatcher.pump();
        }
        CHECK_EQ(remote.factory.resumeCount(), 1u);
        CHECK_EQ(remote.factory.bootstrapCount(), 2u);
        REQUIRE(remote.factory.last());
        remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
        remote.handshake();
        CHECK(remote.core->state() == State::Connected);
        CHECK(remote.disagreement().empty());

        // A successful Welcome keeps a prior QUIC failure on SSH.
        remote.core->suspend();
        remote.core->resumeAfterSuspension(120);
        CHECK_EQ(remote.factory.resumeCount(), 1u);
        CHECK_EQ(remote.factory.bootstrapCount(), 3u);
    }
}

TEST(lifecycleChurnPreservesTheBootstrapRetryAndItsGrowingBackoff)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    remote.core->suspend();
    remote.core->resumeAfterSuspension(120);
    remote.dispatcher.pump();
    REQUIRE(remote.factory.resumeCount() == 1);
    remote.factory.last()->close("the old route is unavailable");
    remote.dispatcher.pump();
    REQUIRE(remote.factory.bootstrapCount() == 2);

    for (unsigned failure = 1; failure <= 8; ++failure) {
        REQUIRE(remote.factory.last());
        remote.factory.last()->close("the bootstrap carrier closed");
        remote.dispatcher.pump();
        REQUIRE(remote.core->state() == State::Reconnecting);
        const auto delay = reconnectDelayForAttempt(failure);
        CHECK_EQ(remote.dispatcher.delaysRequested().back(), delay);
        const auto attempts = remote.factory.bootstrapCount();
        const auto untilRetry = static_cast<std::uint64_t>(delay * second);
        for (std::uint64_t elapsed = 0; elapsed + 100'000 < untilRetry; elapsed += 100'000) {
            remote.core->networkPathChanged();
            remote.core->resumeAfterSuspension(120);
            remote.dispatcher.advance(100'000);
            CHECK_EQ(remote.factory.bootstrapCount(), attempts);
        }
        remote.dispatcher.advance(100'000);
        CHECK_EQ(remote.factory.bootstrapCount(), attempts + 1);
        CHECK_EQ(remote.factory.resumeCount(), 1u);
        CHECK(remote.core->state() == State::Connecting);
    }

    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
}

TEST(aFreshConnectAfterAnInterruptedResumeDoesNotReuseTheFailedCarrier)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    remote.core->suspend();
    remote.core->resumeAfterSuspension(120);
    remote.dispatcher.pump();
    REQUIRE(remote.factory.resumeCount() == 1);
    remote.core->networkPathChanged();
    remote.dispatcher.pump();
    CHECK_EQ(remote.factory.bootstrapCount(), 2u);
    remote.core->stop();
    remote.core.reset();
    REQUIRE(!remote.factory.last());
    const auto bootstraps = remote.factory.bootstrapCount();
    remote.makeCore();
    remote.attach();
    CHECK_EQ(remote.factory.bootstrapCount(), bootstraps + 1);
    CHECK_EQ(remote.factory.resumeCount(), 1u);
    remote.settle(3 * second);
    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.disagreement().empty());
}

TEST(aResumeFactoryFailureOrInterruptionFallsBackWithoutAWelcome)
{
    for (const auto delivery : { StubFactory::ResumeDelivery::Unavailable, StubFactory::ResumeDelivery::Pending }) {
        Remote remote(Bootstrap::QUIC);
        remote.attach();
        remote.factory.setResumeDelivery(delivery);
        remote.core->suspend();
        remote.core->resumeAfterSuspension(120);
        remote.dispatcher.pump();
        if (delivery == StubFactory::ResumeDelivery::Pending) {
            CHECK(!remote.factory.last());
            remote.core->networkPathChanged();
            remote.dispatcher.pump();
            remote.factory.finishPendingResume();
        }
        CHECK_EQ(remote.factory.resumeCount(), 1u);
        CHECK_EQ(remote.factory.bootstrapCount(), 2u);
        REQUIRE(remote.factory.last());
        remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
        remote.handshake();
        remote.settle(3 * second);
        CHECK(remote.core->state() == State::Connected);
        CHECK(remote.disagreement().empty());
    }
}

// Input goes unanswered through a probe, so the attachment declares the
// carrier dead though no transport event says so. The redial begins in that
// same moment: the backoff spaces attempts at a host that refused one, and
// nothing has been refused.
TEST(aRedialAfterDeclaredLostSkipsTheBackoff)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    const std::uint64_t typedAt = remote.dispatcher.now();
    remote.core->sendInput("x");

    std::uint64_t redialledAt = 0;
    for (int step = 0; step < 5'000 && redialledAt == 0; ++step) {
        remote.dispatcher.advance(1'000);
        if (remote.factory.bootstrapCount() == 2)
            redialledAt = remote.dispatcher.now();
    }
    REQUIRE(redialledAt != 0);
    CHECK_MSG(redialledAt - typedAt <= 2 * second, ("the redial began " + std::to_string(redialledAt - typedAt) + " us after the keystroke").c_str());
    CHECK(remote.core->state() == State::Connecting);
    // No reconnect delay was asked for on the way.
    CHECK(std::ranges::none_of(remote.dispatcher.delaysRequested(), [](double delay) { return delay == 1.0; }));

    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
    CHECK(!remote.daemon.host.connection->resumeRefusal().has_value());
}

// A client about to be suspended tells the host, which holds the attachment
// Lost at once, and lets its carrier go. No timer redials a suspended
// client; the resume does, by proof.
TEST(aSuspensionTellsTheHostAndWaitsForTheResume)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    const ID128 attachment = remote.daemon.host.connection->attachment()->attachmentID;

    remote.core->suspend();
    CHECK(remote.core->state() == State::Reconnecting);
    CHECK(remote.factory.last() == nullptr);
    (void)remote.daemon.serve(remote.factory, remote.dispatcher.now());
    CHECK(remote.daemon.host.connection->byeReason() == std::optional { ByeReason::Suspending });
    CHECK(remote.daemon.host.connection->state() == ConnectionState::Closed);

    remote.dispatcher.advance(120 * second);
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
    CHECK_EQ(remote.factory.resumeCount(), 0u);

    remote.print("while suspended\r\n");
    remote.core->resumeAfterSuspension(120.0);
    remote.dispatcher.pump();
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
    CHECK_EQ(remote.factory.resumeCount(), 1u);
    REQUIRE(remote.daemon.acceptProof(remote.factory, remote.dispatcher.now()));
    remote.handshake();
    CHECK(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->attachment()->attachmentID == attachment);
    CHECK(remote.disagreement().empty());

    // A session that is not connected has nothing to suspend.
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    const std::size_t written = remote.factory.sent().size();
    remote.core->suspend();
    CHECK_EQ(remote.factory.sent().size(), written);
}

// The frontend keeps the instance with its window's restoration identity:
// the relaunched window's core is given it, and greets the host as the same
// client. A core given none draws its own, and two never share one.
TEST(theClientInstanceSurvivesARelaunch)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    const ID128 kept = remote.core->clientInstance();
    CHECK(kept != ID128 {});
    CHECK(remote.daemon.host.connection->clientInstance() == kept);

    // The process goes; a new one makes a new core for the restored window.
    remote.core.reset();
    remote.dispatcher.pump();
    remote.makeCore();
    CHECK(remote.core->clientInstance() != kept);
    remote.core->setClientInstance(kept);
    CHECK(remote.core->clientInstance() == kept);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->clientInstance() == kept);
}

// A stall is shown when it begins, which is when nothing arrives: input that
// has waited past its bound for a receipt marks the link, and the host's next
// frame clears it. The link stays connected throughout.
TEST(aStalledLinkIsReportedAsItBeginsAndAsItClears)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    REQUIRE(remote.observer.m_links.empty());
    CHECK(!remote.core->linkState().isStalled());

    remote.core->sendInput("x");
    remote.blackhole(measuredLivenessPolicy().floorMicroseconds - 10'000);
    CHECK(remote.observer.m_links.empty());
    remote.blackhole(20'000);
    REQUIRE(remote.observer.m_links.size() == 1);
    CHECK(remote.observer.m_links.back().isInputUndelivered);
    CHECK(remote.core->linkState().isStalled());
    CHECK(remote.core->state() == State::Connected);

    // The host answers the probe the stall drew.
    remote.settle(100'000);
    REQUIRE(remote.observer.m_links.size() == 2);
    CHECK(!remote.observer.m_links.back().isStalled());
    CHECK(remote.core->state() == State::Connected);
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
}

// A link that stalled and was then declared dead shows no indicator while
// the session reconnects: the state says what happened.
TEST(aLostLinkClearsItsIndicators)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    remote.core->sendInput("x");
    remote.blackhole(3 * second);
    REQUIRE(remote.factory.bootstrapCount() == 2);
    REQUIRE(remote.observer.m_links.size() == 2);
    CHECK(remote.observer.m_links.front().isInputUndelivered);
    CHECK(!remote.observer.m_links.back().isStalled());
    CHECK(!remote.core->linkState().isStalled());

    // The same when the carrier itself reports the loss while the link is
    // marked. What was typed into the dead carrier is in doubt, and new
    // input waits behind it until the person decides.
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    REQUIRE(remote.core->inputStatus().isAwaitingDecision());
    remote.core->discardInDoubt();
    remote.core->sendInput("y");
    remote.blackhole(measuredLivenessPolicy().floorMicroseconds + 10'000);
    REQUIRE(remote.observer.m_links.size() == 3);
    CHECK(remote.core->linkState().isStalled());
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    REQUIRE(remote.observer.m_links.size() == 4);
    CHECK(!remote.observer.m_links.back().isStalled());
    CHECK(!remote.core->linkState().isStalled());
}

// A session whose last handshake rode the stream is not on the QUIC carrier
// the factory once reached, and dials no proof at it: the bootstrap is where
// QUIC is offered again. One attachment crosses the two carriers, each
// greeted with what that carrier can send.
TEST(aSessionAcknowledgedOverAStreamDialsNoProof)
{
    Remote remote(Bootstrap::QUIC);
    remote.attach();
    REQUIRE(remote.core->isUsingQUIC());
    CHECK(remote.factory.last()->datagramsOffered() > 0);
    const ID128 attachment = remote.daemon.host.connection->attachment()->attachmentID;

    // The proof dial dies, and the bootstrap now offers only the stream.
    remote.factory.last()->close("idle timeout");
    remote.dispatcher.pump();
    remote.dispatcher.advance(1'500'000);
    REQUIRE(remote.factory.resumeCount() == 1);
    remote.factory.setBootstrap(Bootstrap::Stream);
    remote.factory.last()->close("no route to host");
    remote.dispatcher.pump();
    REQUIRE(remote.factory.bootstrapCount() == 2);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(!remote.core->isUsingQUIC());
    CHECK_EQ(remote.factory.last()->datagramsOffered(), 0u);
    // The stream resumes the attachment the QUIC carrier began.
    CHECK(remote.daemon.host.connection->attachment()->attachmentID == attachment);

    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.dispatcher.advance(1'500'000);
    CHECK_EQ(remote.factory.resumeCount(), 1u);
    CHECK_EQ(remote.factory.bootstrapCount(), 3u);
}

// A second report of trouble while a proof is awaited is answered by the
// proof already asked for: it neither asks again nor gives the silent
// connection another three seconds.
TEST(aSecondSuspicionDoesNotRestartTheWait)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    const std::size_t writes = remote.factory.sent().size();
    remote.core->networkPathChanged();
    remote.dispatcher.pump();
    const std::size_t writesAfterAsk = remote.factory.sent().size();
    CHECK(writesAfterAsk > writes);
    remote.blackhole(2 * second);
    remote.core->networkPathChanged();
    remote.dispatcher.pump();
    CHECK_EQ(remote.factory.sent().size(), writesAfterAsk);
    CHECK_EQ(remote.factory.bootstrapCount(), 1u);
    remote.blackhole(second + 10'000);
    CHECK_EQ(remote.factory.bootstrapCount(), 2u);
}

// A window resized while its session reconnects greets the host at the new
// size: the resume's facts are the client's as it stands, not as it was when
// the attachment began.
TEST(aResizeWhileReconnectingReachesTheHostOnTheResume)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK_EQ(remote.daemon.host.connection->facts().viewportColumns, 80u);
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.core->setSize(100, 30);
    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(!remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_EQ(remote.daemon.host.connection->facts().viewportColumns, 100u);
    CHECK_EQ(remote.daemon.host.connection->facts().viewportRows, 30u);
}

// The platform says the path is expensive: the host is told the link is
// metered at once, and pushes it no history it did not ask for. The cost
// rides every later greeting, so a reconnect over the same path is metered
// from its first frame; Low Data Mode is constrained, and a free path is
// unmetered again (Docs/sync.md §6.6).
TEST(aMeteredPathSendsFactsLinkMetered)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->facts().link == LinkCost::Unmetered);
    remote.core->setNetworkCost(Ubiquitty::PAL::NetworkCost::Expensive);
    remote.settle();
    CHECK(remote.daemon.host.connection->facts().link == LinkCost::Metered);

    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.daemon.host.connection->facts().link == LinkCost::Metered);

    remote.core->setNetworkCost(Ubiquitty::PAL::NetworkCost::Constrained);
    remote.settle();
    CHECK(remote.daemon.host.connection->facts().link == LinkCost::Constrained);
    remote.core->setNetworkCost(Ubiquitty::PAL::NetworkCost::Unmetered);
    remote.settle();
    CHECK(remote.daemon.host.connection->facts().link == LinkCost::Unmetered);
}

// A pane its frontend keeps hidden is named Dormant by the resume, so the
// host sends it no screen when the attachment continues, whatever was
// printed meanwhile; showing it brings the rows that changed.
TEST(aPaneKeptHiddenCostsNothingOnTheResume)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    remote.core->setView(ViewState::Dormant);
    remote.settle();
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.print("printed while hidden and away\r\n");
    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(!remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_EQ(remote.daemon.host.connection->counters().rowsSent, 0u);
    CHECK_EQ(remote.daemon.host.connection->counters().checkpointsSent, 0u);
    CHECK(!remote.disagreement().empty());

    remote.core->setView(ViewState::Live);
    remote.settle();
    CHECK(remote.disagreement().empty());
    CHECK_EQ(remote.daemon.host.connection->counters().checkpointsSent, 0u);
}

// Input typed into a carrier that was already dead, and input typed while
// the session reconnects, reach the program once each and in order: the
// host's count on the resume says what it wrote, and the rest is sent again.
TEST(inputTypedAcrossADropIsWrittenOnceInOrder)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.core->send(RawInput { .bytes = { 'a' } }));
    remote.settle();
    REQUIRE(remote.daemon.host.pane.writer.bytes() == "a");

    // The path dies with nothing to say so; `b` is written into it.
    CHECK(remote.core->send(RawInput { .bytes = { 'b' } }));
    remote.blackhole(100'000);
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    REQUIRE(remote.core->state() == State::Reconnecting);
    // `c` is typed with no carrier at all, and taken.
    CHECK(remote.core->send(RawInput { .bytes = { 'c' } }));
    CHECK_EQ(remote.core->inputStatus().waiting, 2u);

    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("abc"));
    CHECK_EQ(remote.daemon.host.connection->counters().inputDuplicates, 0u);
    CHECK(!remote.core->inputStatus().isAwaitingDecision());
    CHECK(remote.observer.m_inputs.empty());
}

// The host kept no count of this client's input, so it cannot say whether
// the event typed into the dead carrier was written. It is shown in doubt and
// never sent again unasked; what is typed next waits behind it, and goes
// after it when the person says to send.
TEST(inputTheHostCannotAccountForIsInDoubtUntilThePersonDecides)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.core->send(RawInput { .bytes = { 'x' } }));
    remote.blackhole(100'000);
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);

    REQUIRE(remote.observer.m_inputs.size() == 1);
    CHECK_EQ(remote.observer.m_inputs.back().inDoubt, 1u);
    CHECK(remote.core->inputStatus().isAwaitingDecision());
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string());
    CHECK(remote.core->send(RawInput { .bytes = { 'y' } }));
    remote.settle();
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string());
    CHECK_EQ(remote.core->inputStatus().waiting, 1u);

    remote.core->resendInDoubt();
    remote.settle();
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("xy"));
    REQUIRE(remote.observer.m_inputs.size() == 2);
    CHECK_EQ(remote.observer.m_inputs.back().inDoubt, 0u);
    CHECK(!remote.core->inputStatus().isAwaitingDecision());

    // Discarding is the other answer: the event is counted, never written.
    // The host has forgotten the attachment, so the resume is refused, and
    // only `w`, which the dead carrier took, is in doubt: `v`, typed with no
    // carrier at all, waits behind it and goes after the decision.
    CHECK(remote.core->send(RawInput { .bytes = { 'z' } }));
    remote.settle();
    CHECK(remote.core->send(RawInput { .bytes = { 'w' } }));
    remote.blackhole(100'000);
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.daemon.host.lose(remote.dispatcher.now());
    remote.daemon.host.table.expire(remote.dispatcher.now() + measuredResumePolicy().lostGraceMicroseconds + second);
    CHECK(remote.core->send(RawInput { .bytes = { 'v' } }));
    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->inputStatus().inDoubt == 1);
    CHECK_EQ(remote.core->inputStatus().waiting, 1u);
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("xyz"));
    remote.core->discardInDoubt();
    remote.settle();
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("xyzv"));
    CHECK_EQ(remote.core->inputStatus().discarded, 1u);
    CHECK_EQ(remote.observer.m_inputs.back().discarded, 1u);
}

// What is typed while the session reconnects never left this client, so a
// resume the host refuses puts none of it in doubt and the frontend is told
// nothing: it reaches the program once the host binds the pane again.
TEST(inputTypedWithNoCarrierGoesWhenARefusedResumeBindsThePaneAgain)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.core->send(RawInput { .bytes = { 'a' } }));
    remote.settle();
    REQUIRE(remote.daemon.host.pane.writer.bytes() == "a");
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    remote.daemon.host.lose(remote.dispatcher.now());
    remote.daemon.host.table.expire(remote.dispatcher.now() + measuredResumePolicy().lostGraceMicroseconds + second);
    CHECK(remote.core->send(RawInput { .bytes = { 'b' } }));
    CHECK_EQ(remote.core->inputStatus().waiting, 1u);

    remote.dispatcher.advance(1'500'000);
    remote.daemon.acceptBootstrapped(remote.factory, remote.dispatcher.now());
    remote.handshake();
    REQUIRE(remote.core->state() == State::Connected);
    REQUIRE(remote.daemon.host.connection->resumeRefusal().has_value());
    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("ab"));
    CHECK(!remote.core->inputStatus().isAwaitingDecision());
    CHECK_EQ(remote.core->inputStatus().waiting, 0u);
    CHECK(remote.observer.m_inputs.empty());
    CHECK_EQ(remote.daemon.host.connection->counters().inputDuplicates, 0u);
}

// The pane's program is gone when the session comes back: what was typed for
// it is discarded and said, and nothing more is taken for it.
TEST(inputForAPaneThatEndedIsDiscardedAndSaid)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    CHECK(remote.core->send(RawInput { .bytes = { 'a' } }));
    remote.settle();
    remote.factory.last()->close("connection reset");
    remote.dispatcher.pump();
    CHECK(remote.core->send(RawInput { .bytes = { 'b' } }));
    remote.daemon.host.hasPane = false;
    remote.dispatcher.advance(1'500'000);
    remote.daemon.host.lose(remote.dispatcher.now());
    remote.daemon.host.accept();
    remote.daemon.adopt(remote.factory);
    remote.handshake();

    CHECK_STR_EQ(remote.daemon.host.pane.writer.bytes(), std::string("a"));
    REQUIRE(!remote.observer.m_inputs.empty());
    CHECK_EQ(remote.observer.m_inputs.back().discarded, 1u);
    CHECK(!remote.core->send(RawInput { .bytes = { 'c' } }));
}

// Bytes longer than one event go as several, in order, and arrive whole.
TEST(aLongRunOfBytesGoesAsSeveralEventsInOrder)
{
    Remote remote(Bootstrap::Stream);
    remote.attach();
    REQUIRE(remote.core->state() == State::Connected);
    std::string typed;
    for (int index = 0; index < 40'000; ++index)
        typed.push_back(static_cast<char>('a' + index % 26));
    remote.core->sendInput(typed);
    remote.settle(second);
    CHECK(remote.daemon.host.pane.writer.bytes() == typed);
    CHECK(remote.daemon.host.pane.writer.writes().size() >= 3);
}

UBIQUITTY_TEST_MAIN()
