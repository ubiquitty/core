// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "UbiquittyKit/Session.h"

#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Session/HealthBadge.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/InboundBackpressure.h"
#include "Core/Transport/LocalPTYChannel.h"

#include <array>
#include <cstring>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace UbiquittyKit {

using Ubiquitty::Session::Health;

namespace {

/// A configuration change on its way to the session thread.
///
/// Boxed when posted: the values and the renderer's resolution of them are
/// larger than a posted unit holds.
struct ConfigurationChange {
    Ubiquitty::Preferences::Values values;
    Ubiquitty::Rendering::RenderConfiguration rendering;
};

} // namespace

// ---- Direct origin ---------------------------------------------------------
//
// A carrier whose bytes are themselves the terminal stream, with no Sync framing
// above it. A local PTY is one such carrier; an SSH channel running a login shell
// is another, which is why this state is not named for either. Whether the bytes
// crossed a machine boundary is the carrier's own `origin()` to report, never a
// branch here.
//
// The carrier's I/O thread delivers the bytes; every byte is marshalled onto the
// session thread before it touches the Terminal. Marshalled work captures a WEAK
// reference and re-checks hasStopped on the session thread: the caller-owned
// Dispatcher can still hold queued work after the state is stopped or destroyed,
// and that work must never touch freed state, publish health, or mutate the
// Terminal.
struct Session::LocalState final : private Ubiquitty::Transport::ChannelObserver, std::enable_shared_from_this<Session::LocalState> {
    // `applyWindowSize` carries geometry to whatever the carrier's own resize
    // is — TIOCSWINSZ on a PTY, a `window-change` request on an SSH shell.
    // Channel itself does not spell resize, because most carriers have no
    // geometry to resize.
    LocalState(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Observer& observer,
        std::unique_ptr<Ubiquitty::Transport::Channel> channel, std::function<void(int, int)> applyWindowSize)
        : terminal(terminal)
        , dispatcher(dispatcher)
        , observer(observer)
        , channel(std::move(channel))
        , applyWindowSize(std::move(applyWindowSize))
    {
    }

    void start(int columns, int rows)
    {
        if (hasStarted)
            return;
        hasStarted = true;
        Ubiquitty::resize(terminal, rows, columns);
        if (applyWindowSize)
            applyWindowSize(columns, rows);
        if (!channel->start(this)) {
            health = Health::Failed;
            observer.onHealthChanged(health, "carrier failed to start");
            observer.onExit();
        }
    }

    void stop()
    {
        hasStopped = true; // session thread; queued work re-checks this there
        channel->stop(); // no onClose follows a caller stop()
        health = Health::Failed;
    }

    void setSize(int columns, int rows)
    {
        Ubiquitty::resize(terminal, rows, columns);
        if (applyWindowSize)
            applyWindowSize(columns, rows);
        observer.onFrame();
    }

    // ChannelObserver — I/O thread; marshal everything through a weak
    // reference + the hasStopped re-check (see the struct comment).
    void onConnect() override
    {
        dispatcher.post([weakSelf = weak_from_this()] {
            auto self = weakSelf.lock();
            if (!self || self->hasStopped)
                return;
            self->health = Health::Connected;
            self->observer.onHealthChanged(self->health, std::string());
        });
    }

    void onData(std::span<const std::uint8_t> data) override
    {
        const auto* chars = reinterpret_cast<const char*>(data.data());
        std::vector<char> bytes(chars, chars + data.size());
        // What this consumer holds and has not parsed. The copy below travels
        // to the session thread, so between the post and the parse it is queue
        // depth that nothing else bounds — and the I/O thread's next act is to
        // read more. Counted here and released on the other side, so the
        // carrier is paced by the work outstanding rather than by the rate.
        const std::size_t stagedBytes = queuedBytes.fetch_add(bytes.size(), std::memory_order_acq_rel) + bytes.size();
        dispatcher.post([weakSelf = weak_from_this(), bytes = std::move(bytes)] {
            auto self = weakSelf.lock();
            if (!self)
                return;
            const std::size_t remaining = self->queuedBytes.fetch_sub(bytes.size(), std::memory_order_acq_rel) - bytes.size();
            self->inboundBackpressure.applyTo(self->channel.get(), remaining);
            if (self->hasStopped)
                return;
            Ubiquitty::processBytes(&self->terminal, std::string_view(bytes));
            // A query the application sent — DA1, DSR, DECRQSS — leaves its
            // answer in the terminal's reply buffer, and the buffer is pull
            // model: whoever fed the bytes owes the delivery. Unclaimed, the
            // application waits forever on a reply that was written and never
            // sent, which is a hang rather than a missing feature.
            self->flushReplies();
            self->observer.onFrame();
            // Origin parity: title/bell reach the same hooks as remote. The
            // Terminal latches both from the byte stream; consume the latch.
            if (self->terminal.titleChanged) {
                self->terminal.titleChanged = false;
                self->observer.onTitle(self->terminal.titleUTF8);
            }
            if (self->terminal.bellPending) {
                self->terminal.bellPending = false;
                self->observer.onBell();
            }
        });
        inboundBackpressure.applyTo(channel.get(), stagedBytes);
    }

    // Drains the terminal's reply buffer onto the channel the bytes came from.
    void flushReplies()
    {
        std::array<char, 4096> reply;
        int written = 0;
        while ((written = Ubiquitty::drainOutput(&terminal, { reply.data(), reply.size() })) > 0)
            channel->send({ reinterpret_cast<const std::uint8_t*>(reply.data()), static_cast<std::size_t>(written) });
    }

    void onClose(const std::string& reason) override
    {
        dispatcher.post([weakSelf = weak_from_this(), reason = std::string(reason)] {
            auto self = weakSelf.lock();
            if (!self || self->hasStopped)
                return;
            self->health = Health::Failed;
            self->observer.onHealthChanged(self->health, reason);
            self->observer.onExit();
        });
    }

    Ubiquitty::Terminal& terminal;
    Ubiquitty::Session::Dispatcher& dispatcher;
    // Bytes handed to the dispatcher and not yet parsed. Atomic because the
    // I/O thread adds and the session thread subtracts.
    std::atomic<std::size_t> queuedBytes { 0 };
    // The two marks come from Core, so this consumer and the remote one cannot
    // answer differently how much a slow session may buffer.
    Ubiquitty::Transport::InboundBackpressure inboundBackpressure;
    Observer& observer;
    std::unique_ptr<Ubiquitty::Transport::Channel> channel;
    std::function<void(int, int)> applyWindowSize;
    Health health = Health::Connecting;
    bool hasStarted = false;
    bool hasStopped = false;
};

// ---- Remote origin ---------------------------------------------------------
struct Session::RemoteState final : private Ubiquitty::Session::RemoteSessionObserver {
    RemoteState(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Ubiquitty::Session::ChannelFactory& factory, Observer& observer, std::uint32_t attachSessionID)
        : observer(observer)
        , attachSessionID(attachSessionID)
        , core(&terminal, &dispatcher, &factory, this)
    {
    }

    // RemoteSessionObserver — already on the session thread.
    void onStateChanged(Ubiquitty::Session::State state, const std::string& detail) override
    {
        // Core's fold, not a second one: a kit that mapped the phases itself
        // would put a window's badge and a phone's on different vocabularies
        // the first time either was edited.
        health = Ubiquitty::Session::healthFor(state);
        observer.onHealthChanged(health, detail);
    }
    void onScreenChanged() override { observer.onFrame(); } // coverage-exempt(conventions): pure one-line forward over the separately-tested RemoteSessionCore
    void onTitle(std::string_view utf8) override { observer.onTitle(utf8); } // coverage-exempt(conventions): pure one-line forward over the separately-tested RemoteSessionCore
    void onBell() override { observer.onBell(); }
    void onExit() override { observer.onExit(); }
    void onInDoubt(const Ubiquitty::Protocol::Sync::PaneInputStatus&) override { observer.onInputInDoubt(); }

    Observer& observer;
    std::uint32_t attachSessionID = 0;
    Health health = Health::Connecting;
    Ubiquitty::Session::RemoteSessionCore core;
};

// ---- The facade ------------------------------------------------------------

Session::Session() = default;

Session::~Session()
{
    m_configurationSubscription.reset();
    m_lifetime.reset();
    stop();
}

void Session::followConfiguration(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Observer& observer)
{
    m_lifetime = std::make_shared<int>(0);
    // A remote terminal is fed bytes the host already decoded; only a carrier
    // whose bytes are the stream itself follows the configured encoding.
    const auto encoding = m_remote ? Ubiquitty::Runtime::EncodingReseed::KeepCurrent : Ubiquitty::Runtime::EncodingReseed::FollowConfiguration;
    m_configurationSubscription = std::make_unique<Ubiquitty::Preferences::ConfigurationSubscription>(
        [&terminal, &dispatcher, &observer, encoding, lifetime = std::weak_ptr<int>(m_lifetime)](const Ubiquitty::Preferences::Values& values) {
            // Derived here, on the main thread the configuration lives on, and
            // carried to the session thread by value.
            auto change = std::make_unique<ConfigurationChange>(values, Ubiquitty::Preferences::Effective::renderConfiguration(values));
            dispatcher.post([&terminal, &observer, encoding, lifetime, change = std::move(change)] {
                if (lifetime.expired())
                    return;
                Ubiquitty::Runtime::reseedTerminalConfiguration(terminal, change->values, encoding);
                observer.onConfiguration(change->rendering);
                observer.onFrame();
            });
        });
}

std::unique_ptr<Session> Session::createLocal(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Observer& observer, std::unique_ptr<Ubiquitty::PAL::Stream> pty)
{
    auto channel = std::make_unique<Ubiquitty::Transport::LocalPTYChannel>(std::move(pty));
    // Captured raw: the LocalState owns the channel and outlives every call.
    auto* ptyChannel = channel.get();
    std::unique_ptr<Session> session(new Session());
    session->m_local = std::make_shared<LocalState>(terminal, dispatcher, observer, std::move(channel),
        [ptyChannel](int columns, int rows) { ptyChannel->setWindowSize(columns, rows); });
    session->followConfiguration(terminal, dispatcher, observer);
    return session;
}

std::unique_ptr<Session> Session::createDirect(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Observer& observer,
    std::unique_ptr<Ubiquitty::Transport::Channel> channel, std::function<void(int, int)> applyWindowSize)
{
    std::unique_ptr<Session> session(new Session());
    session->m_local = std::make_shared<LocalState>(terminal, dispatcher, observer, std::move(channel), std::move(applyWindowSize));
    session->followConfiguration(terminal, dispatcher, observer);
    return session;
}

std::unique_ptr<Session> Session::createRemote(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Ubiquitty::Session::ChannelFactory& factory, Observer& observer, std::uint32_t attachSessionID)
{
    std::unique_ptr<Session> session(new Session());
    session->m_remote = std::make_unique<RemoteState>(terminal, dispatcher, factory, observer, attachSessionID);
    session->followConfiguration(terminal, dispatcher, observer);
    return session;
}

void Session::start(int columns, int rows)
{
    if (m_local)
        m_local->start(columns, rows);
    else if (m_remote)
        m_remote->core.start(m_remote->attachSessionID, columns, rows);
}

void Session::stop()
{
    if (m_local)
        m_local->stop();
    else if (m_remote)
        m_remote->core.stop();
}

void Session::sendInput(std::string_view bytes)
{
    if (bytes.empty())
        return;
    if (m_local) {
        // No-op before start (and after stop) — matching the remote origin's
        // no-op-until-up contract; the carrier would otherwise defer the bytes.
        if (!m_local->hasStarted || m_local->hasStopped)
            return;
        // The one place input crosses from the caller's char to the wire's
        // octet. Both spell a byte; only the wire's spelling is the one every
        // Sync buffer is written in, so the reinterpretation happens here
        // rather than in each carrier.
        m_local->channel->send({ reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    } else if (m_remote)
        m_remote->core.sendInput(bytes);
}

void Session::setSize(int columns, int rows)
{
    if (m_local)
        m_local->setSize(columns, rows);
    else if (m_remote)
        m_remote->core.setSize(columns, rows);
}

Ubiquitty::Session::InputInDoubtNotice Session::inputInDoubtNotice() const
{
    if (!m_remote)
        return {};
    return Ubiquitty::Session::inputInDoubtNotice(m_remote->core.inputStatus()).value_or(Ubiquitty::Session::InputInDoubtNotice {});
}

void Session::resendInputInDoubt()
{
    if (m_remote)
        m_remote->core.resendInDoubt();
}

void Session::discardInputInDoubt()
{
    if (m_remote)
        m_remote->core.discardInDoubt();
}

Ubiquitty::Transport::Origin Session::origin() const
{
    // The carrier's own stamp: a PTY says Local, an SSH shell says SSH. Reading
    // it here is what keeps a new direct carrier from needing a case in the
    // facade.
    if (m_local)
        return m_local->channel->origin();
    // The daemon host string is the embedder's ChannelFactory's knowledge; the
    // facade reports the kind.
    return Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, std::string() };
}

Health Session::health() const
{
    if (m_local)
        return m_local->health;
    if (m_remote)
        return m_remote->health;
    return Health::Failed;
}

} // namespace UbiquittyKit
