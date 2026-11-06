// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_pane_link_conformance — the in-process link, the remote link and the
// loopback link each keep the one `PaneLink` contract
// (Tests/Core/Session/PaneLinkConformance.h).
//
// Each fixture attaches its link to a program that writes back what it is
// written:
//
// - the in-process link is a TerminalSession's own, over a stream double whose
//   writes the fixture reads back to it;
// - the remote link is a RemoteSessionCore over a stub carrier to the host's
//   real attachment table and connection, on one clock with its dispatcher;
// - the loopback link is `LoopbackPaneLink`, the oracle's two connection ends.
//
// The remote link against a real daemon runs the same checks in the
// remote_pane_link_e2e stanza.

#include "Core/Protocol/Sync/AttachmentLink.h"
#include "Core/Session/PaneLinkConformance.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "Platform/StreamDouble.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Session/PaneLink.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/Channel.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// Lends the conformance suite a session's in-process link.
struct PaneLinkTestAccess {
    /// The link the session routes its pane through.
    ///
    /// - Parameter session: The session.
    /// - Returns: Its link, borrowed for the session's life.
    static Session::PaneLink& link(TerminalSession& session) { return session.paneLink(); }
};

} // namespace Ubiquitty::Runtime

namespace {

namespace Session = Ubiquitty::Session;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Link = UbiquittyTest::AttachmentLink;
using Ubiquitty::Runtime::TerminalSession;

/// A TerminalSession's own link, over a stream double that writes back what
/// the session writes to it. Nothing ingests for the session: the fixture
/// drains it and asks for its held frames, as a frontend that owns its drain
/// loop does.
class LocalFixture {
public:
    LocalFixture()
        : m_session(TerminalSession::make())
    {
        if (!m_session)
            return;
        m_session->start(24, 80);
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        m_stream = stream.get();
        m_stream->pushInbound("$ ");
        m_isReady = m_session->adoptStream(std::move(stream)).has_value();
        // A refused stream went back with the refusal.
        if (!m_isReady) {
            m_stream = nullptr;
            return;
        }
        settle();
    }

    /// Whether the session started and took its stream.
    [[nodiscard]] bool isReady() const { return m_isReady; }

    /// The session's link.
    [[nodiscard]] Session::PaneLink& link() { return Ubiquitty::Runtime::PaneLinkTestAccess::link(*m_session); }

    /// Answers what the session wrote, drains, and asks for a held frame,
    /// until a condition holds or the passes run out. The coherence rule
    /// reads the steady clock, so a pass waits a millisecond.
    ///
    /// - Parameter isDone: Whether the wait is over.
    /// - Returns: Whether it held.
    bool runUntil(const std::function<bool()>& isDone)
    {
        for (int pass = 0; pass < 2'000; ++pass) {
            answer();
            if (isDone())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return isDone();
    }

    /// Answers, drains and asks for a held frame for a few passes.
    void settle()
    {
        for (int pass = 0; pass < 20; ++pass) {
            answer();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /// Reads the view under the session's model lock.
    ///
    /// - Parameter reader: Reads the view while the lock is held.
    void readView(const std::function<void(const Session::PaneView&)>& reader)
    {
        m_session->lockForRead();
        reader(link().view());
        m_session->unlockForRead();
    }

private:
    /// Hands the session back what it wrote since the last answer, as an
    /// echoing program would, drains it, and asks for the frame the drain
    /// held.
    ///
    /// The drain that parses the echo asks the coherence rule before the read
    /// has gone quiet, so the rule holds the frame, and a drain that reads
    /// nothing does not ask again. A frontend that drains on its own
    /// readiness asks again when the wait it was given is up, as the ingest
    /// loop does; asking on every pass is never later than that.
    void answer()
    {
        const std::string& written = m_stream->written;
        if (written.size() > m_answered) {
            m_stream->pushInbound(std::string_view(written).substr(m_answered));
            m_answered = written.size();
        }
        (void)m_session->drain();
        (void)m_session->publishHeldFrameIfDue();
    }

    /// Releases the session's reference.
    struct Release {
        void operator()(TerminalSession* session) const { releaseTerminalSession(session); }
    };

    std::unique_ptr<TerminalSession, Release> m_session;
    Ubiquitty::TestDoubles::StreamDouble* m_stream = nullptr;
    std::size_t m_answered = 0;
    bool m_isReady = false;
};

/// Runs posted work in order and delayed work when the clock reaches it, and
/// is the clock both ends of the remote link read.
class ClockedDispatcher final : public Session::Dispatcher {
public:
    /// Queues work for the next pump.
    ///
    /// - Parameter work: The work.
    void post(Session::PostedWork work) override { m_queue.push_back(std::move(work)); }

    /// Queues work for when the clock has moved on by a delay.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: The work.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        m_delayed.push_back({ .due = m_now + static_cast<std::uint64_t>(seconds * 1'000'000.0), .work = std::move(work) });
    }

    /// Runs the posted work, and what it posts.
    void pump()
    {
        while (!m_queue.empty()) {
            Session::PostedWork work = std::move(m_queue.front());
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
            Session::PostedWork work = std::move(next->work);
            m_delayed.erase(next);
            std::move(work)();
            pump();
        }
        m_now = until;
    }

    /// The clock, in microseconds.
    [[nodiscard]] std::uint64_t now() const { return m_now; }

private:
    /// Work waiting for its time.
    struct Delayed {
        std::uint64_t due = 0;
        Session::PostedWork work;
    };

    std::vector<Session::PostedWork> m_queue;
    std::vector<Delayed> m_delayed;
    std::uint64_t m_now = 100'000'000;
};

/// A carrier that keeps what the core writes and lets the fixture speak as its
/// I/O thread.
class StubChannel final : public Ubiquitty::Transport::Channel {
public:
    /// - Parameter sent: Where the core's writes are kept; it outlives the
    ///   channel.
    explicit StubChannel(std::vector<std::vector<std::uint8_t>>& sent)
        : m_sent(sent)
    {
    }

    /// Keeps the observer; the carrier connects when the fixture says so.
    ///
    /// - Parameter observer: The core.
    /// - Returns: True: the carrier started.
    bool start(Ubiquitty::Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }

    /// Keeps one write.
    ///
    /// - Parameter data: The bytes.
    void send(std::span<const std::uint8_t> data) override { m_sent.emplace_back(data.begin(), data.end()); }

    /// Stops nothing: the fixture owns the carrier's other end.
    void stop() override { }

    /// Reaches a daemon.
    ///
    /// - Returns: The origin.
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const override { return { Ubiquitty::Transport::OriginKind::Daemon, "" }; }

    /// Is always connected.
    ///
    /// - Returns: Connected.
    [[nodiscard]] Ubiquitty::Transport::Health health() const override { return Ubiquitty::Transport::Health::Connected; }

    /// Describes the carrier.
    ///
    /// - Returns: Its name.
    [[nodiscard]] std::string description() const override { return "stub"; }

    /// Reports the carrier connected.
    void connect() { m_observer->onConnect(); }

    /// Hands the core bytes from the host.
    ///
    /// - Parameter bytes: The bytes.
    void deliver(std::span<const std::uint8_t> bytes) { m_observer->onData(bytes); }

private:
    std::vector<std::vector<std::uint8_t>>& m_sent;
    Ubiquitty::Transport::ChannelObserver* m_observer = nullptr;
};

/// Builds stub carriers, keeping every write of every one in order.
class StubFactory final : public Session::ChannelFactory {
public:
    /// Builds a carrier.
    ///
    /// - Parameter done: Receives the carrier.
    void createChannel(Session::TransportMode, Done done) override
    {
        auto channel = std::make_unique<StubChannel>(m_sent);
        m_last = channel.get();
        done(std::move(channel), false);
    }

    /// The carrier the core holds, or null.
    [[nodiscard]] StubChannel* last() const { return m_last; }

    /// Every write, in order.
    [[nodiscard]] const std::vector<std::vector<std::uint8_t>>& sent() const { return m_sent; }

private:
    StubChannel* m_last = nullptr;
    std::vector<std::vector<std::uint8_t>> m_sent;
};

/// A RemoteSessionCore over a stub carrier to the host's real attachment
/// table and connection, whose pane's program writes back what it is
/// written.
class RemoteFixture {
public:
    RemoteFixture()
    {
        m_core.emplace(m_terminal.get(), &m_dispatcher, &m_factory, &m_observer);
        m_core->setClock([this] { return m_dispatcher.now(); });
        m_core->start(0, 80, 24);
        m_host.accept();
        m_host.connection->setGreetingAdmission([](const Sync::Greet& greet) -> std::expected<Sync::HostIdentity, Sync::Bye> {
            const auto& request = greet.request;
            if (request.target != Sync::AdmissionTarget::Session || request.workspaceID || (request.sessionID && request.sessionID != sessionID))
                return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone });
            return Sync::HostIdentity {
                .principal = Link::owner.principal,
                .mayDrive = Link::owner.mayDrive,
                .verifiedKind = Link::owner.verifiedKind,
                .selection = { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .daemonInstanceID = daemonInstanceID, .socketName = "conformance.sock", .hostPlatform = "fixture" },
            };
        });
        if (!m_factory.last())
            return;
        m_factory.last()->connect();
        m_dispatcher.pump();
        settle();
    }

    /// Whether the core attached and its pane is bound.
    [[nodiscard]] bool isReady() const { return m_core && m_core->acknowledged() && m_isPaneBound; }

    /// The core, which is the remote link.
    [[nodiscard]] Session::PaneLink& link() { return *m_core; }

    /// Carries what each end has for the other and answers the program's
    /// input, a millisecond of the shared clock at a time, until a condition
    /// holds or two seconds of it pass.
    ///
    /// - Parameter isDone: Whether the wait is over.
    /// - Returns: Whether it held.
    bool runUntil(const std::function<bool()>& isDone)
    {
        for (int tick = 0; tick < 2'000; ++tick) {
            step();
            if (isDone())
                return true;
        }
        return isDone();
    }

    /// Carries and answers for 200 ms of the shared clock.
    void settle()
    {
        for (int tick = 0; tick < 200; ++tick)
            step();
    }

    /// Reads the view; the core's terminal is written only on this thread.
    ///
    /// - Parameter reader: Reads the view.
    void readView(const std::function<void(const Session::PaneView&)>& reader) { reader(m_core->view()); }

private:
    /// The session and daemon instance the host names: the pane is
    /// `Link::paneID`, which the host mints from the two.
    static constexpr std::uint32_t sessionID = 9;
    static constexpr std::uint64_t daemonInstanceID = 10;

    /// Counts nothing: the conformance checks read the link itself.
    class Quiet final : public Session::RemoteSessionObserver { };

    /// Answers the program's input, carries both ways once, and moves the
    /// clock a millisecond.
    void step()
    {
        const std::string& written = m_host.pane.writer.bytes();
        if (written.size() > m_answered) {
            UbiquittyTest::feed(m_host.pane.terminal, std::string_view(written).substr(m_answered));
            m_answered = written.size();
            m_host.pane.source->publish(Sync::FrameClass::Settled, m_dispatcher.now());
        }
        carry();
        m_dispatcher.advance(1'000);
    }

    /// Hands the host what the core wrote, and the core what the host sends.
    void carry()
    {
        const std::uint64_t now = m_dispatcher.now();
        for (; m_consumed < m_factory.sent().size(); ++m_consumed)
            m_host.connection->receiveStream(m_factory.sent()[m_consumed], now);
        if (!m_isPaneBound && m_host.connection->state() == Sync::ConnectionState::Attached)
            m_isPaneBound = m_host.connection->bindPane(*m_host.pane.source) != 0;
        m_host.connection->service(now);
        std::vector<std::uint8_t> down;
        std::vector<std::uint8_t> frame;
        while (m_host.connection->popStreamFrame(frame, now)) {
            down.insert(down.end(), frame.begin(), frame.end());
            frame.clear();
        }
        if (!down.empty() && m_factory.last()) {
            m_factory.last()->deliver(down);
            m_dispatcher.pump();
        }
    }

    std::unique_ptr<Ubiquitty::Terminal> m_terminal = std::make_unique<Ubiquitty::Terminal>();
    ClockedDispatcher m_dispatcher;
    StubFactory m_factory;
    Quiet m_observer;
    Link::Host m_host { { 41, 42 } };
    std::size_t m_consumed = 0;
    std::size_t m_answered = 0;
    bool m_isPaneBound = false;
    // Destroyed first: it holds the dispatcher, the factory and the terminal.
    std::optional<Session::RemoteSessionCore> m_core;
};

/// The loopback link, whose host terminal is fed back what its program is
/// written.
class LoopbackFixture {
public:
    LoopbackFixture() { settle(); }

    /// Whether the oracle attached its client.
    [[nodiscard]] bool isReady() const { return m_link.failure().empty(); }

    /// The loopback link.
    [[nodiscard]] Session::PaneLink& link() { return m_link; }

    /// Answers the program's input until a condition holds or the passes run
    /// out.
    ///
    /// - Parameter isDone: Whether the wait is over.
    /// - Returns: Whether it held.
    bool runUntil(const std::function<bool()>& isDone)
    {
        for (int pass = 0; pass < 200; ++pass) {
            answer();
            if (isDone())
                return true;
        }
        return isDone();
    }

    /// Answers for a few passes.
    void settle()
    {
        for (int pass = 0; pass < 4; ++pass)
            answer();
    }

    /// Reads the view; the loopback has no lock to take.
    ///
    /// - Parameter reader: Reads the view.
    void readView(const std::function<void(const Session::PaneView&)>& reader) { reader(m_link.view()); }

private:
    /// Feeds the host terminal what its program was written since the last
    /// answer, as an echoing program would, and publishes the frame.
    void answer()
    {
        const std::string& written = m_link.programInput();
        if (written.size() <= m_answered)
            return;
        m_link.feedHost(std::string_view(written).substr(m_answered));
        m_answered = written.size();
        m_link.publish(Ubiquitty::Runtime::FrameClass::Settled);
    }

    UbiquittyTest::LoopbackPaneLink m_link;
    std::size_t m_answered = 0;
};

} // namespace

// The link a local terminal's pane is drawn through.
TEST(theInProcessLinkKeepsThePaneLinkContract)
{
    LocalFixture fixture;
    REQUIRE(fixture.isReady());
    UbiquittyTest::checkPaneLinkConformance(fixture, "in-process link");
}

// The link a remote pane is drawn through, against the host's own connection.
TEST(theRemoteLinkKeepsThePaneLinkContract)
{
    RemoteFixture fixture;
    REQUIRE(fixture.isReady());
    UbiquittyTest::checkPaneLinkConformance(fixture, "remote link");
}

// The oracle's link, which stands in for a remote one in the tests.
TEST(theLoopbackLinkKeepsThePaneLinkContract)
{
    LoopbackFixture fixture;
    REQUIRE(fixture.isReady());
    UbiquittyTest::checkPaneLinkConformance(fixture, "loopback link");
}

UBIQUITTY_TEST_MAIN()
