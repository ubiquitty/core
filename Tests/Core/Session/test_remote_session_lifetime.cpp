// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_remote_session_lifetime — work handed out before the session went away
// must not run against it afterwards.
//
// RemoteSessionCore hands closures to two things that outlive a single call:
// the factory's completion, and the dispatcher's queue. The generation counter
// cannot guard either, because reading it is itself a read through the object
// that may be gone. Tearing the channel down joins the I/O thread, which ends
// callbacks that have not started and recalls nothing already handed over.
//
// The owner is a pane a person can close, and a failed connect arms a reconnect
// up to reconnectMaxDelaySeconds out with the core still named by the timer.
//
// Registered with CMake rather than written as a Host stanza on purpose: the
// sanitized driver instruments exactly what CMake registers, so a use-after-free
// here is caught by the checker and a stanza's would not be.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/SessionDirectoryCore.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using Ubiquitty::Session::ChannelFactory;
using Ubiquitty::Session::Dispatcher;
using Ubiquitty::Session::PostedWork;
using Ubiquitty::Session::RemoteSessionCore;
using Ubiquitty::Session::RemoteSessionObserver;
using Ubiquitty::Session::TransportMode;

/// A dispatcher that runs nothing until the case says so.
struct QueueingDispatcher final : Dispatcher {
    std::vector<PostedWork> queued;
    std::vector<PostedWork> delayed;

    void post(PostedWork work) override { queued.push_back(std::move(work)); }
    void postDelayed(double, PostedWork work) override { delayed.push_back(std::move(work)); }

    /// Runs what a deadline would have run, as the platform's run loop would.
    void fireDelayed()
    {
        std::vector<PostedWork> running;
        running.swap(delayed);
        for (auto& work : running)
            std::move(work)();
    }

    /// Runs what was queued, as the platform's run loop would.
    void drain()
    {
        std::vector<PostedWork> running;
        running.swap(queued);
        for (auto& work : running)
            std::move(work)();
    }
};

/// A factory that answers with no transport, which is what arms a reconnect.
struct RefusingFactory final : ChannelFactory {
    int createCount = 0;

    void createChannel(TransportMode, Done done) override
    {
        ++createCount;
        done(nullptr, false);
    }
};

/// A factory that keeps the completion instead of calling it, so a case can
/// answer an attempt after its owner is gone.
struct HoldingFactory final : ChannelFactory {
    Done held;
    std::optional<Ubiquitty::Session::ChannelCreationFailure> failure;

    void createChannel(TransportMode, Done done) override { held = std::move(done); }
    std::optional<Ubiquitty::Session::ChannelCreationFailure> creationFailure() const override { return failure; }
};

/// An observer that counts, so the case can say nothing reached it.
struct CountingObserver final : RemoteSessionObserver {
    int stateChanges = 0;
    int exits = 0;
    int establishes = 0;
    Ubiquitty::Session::State lastState = Ubiquitty::Session::State::Idle;
    std::string lastDetail;

    void onStateChanged(Ubiquitty::Session::State state, const std::string& detail) override
    {
        ++stateChanges;
        lastState = state;
        lastDetail = detail;
    }
    void onExit() override { ++exits; }
    void onSessionEstablished(std::uint32_t) override { ++establishes; }
};

struct FailingFactory final : ChannelFactory {
    Ubiquitty::Session::ChannelCreationFailure failure;
    unsigned requests = 0;
    TransportMode preferredControl = TransportMode::TCP;
    std::optional<TransportMode> requestedTransport;

    void createChannel(TransportMode transport, Done done) override
    {
        ++requests;
        requestedTransport = transport;
        done(nullptr, false);
    }
    std::optional<Ubiquitty::Session::ChannelCreationFailure> creationFailure() const override { return failure; }
    TransportMode controlTransport() const override { return preferredControl; }
};

struct AuthenticationRefusalChannel final : Ubiquitty::Transport::Channel {
    Ubiquitty::Transport::ChannelObserver* observer = nullptr;
    bool shouldStart = true;

    bool start(Ubiquitty::Transport::ChannelObserver* suppliedObserver) override
    {
        observer = suppliedObserver;
        return shouldStart;
    }
    void send(std::span<const std::uint8_t>) override { }
    void stop() override { observer = nullptr; }
    Ubiquitty::Transport::Origin origin() const override { return {}; }
    Ubiquitty::Transport::Health health() const override { return Ubiquitty::Transport::Health::Failed; }
    std::string description() const override { return "mock authentication refusal"; }
    bool peerAuthenticationFailed() const override { return true; }
};

struct AuthenticationRefusalFactory final : ChannelFactory {
    AuthenticationRefusalChannel* channel = nullptr;
    unsigned requests = 0;
    bool shouldStart = true;
    void createChannel(TransportMode mode, Done done) override
    {
        CHECK(mode == TransportMode::QUIC);
        ++requests;
        auto next = std::make_unique<AuthenticationRefusalChannel>();
        next->shouldStart = shouldStart;
        channel = next.get();
        done(std::move(next), true);
    }
};

struct BoundedSetupChannel final : Ubiquitty::Transport::Channel {
    Ubiquitty::Transport::ChannelObserver* observer = nullptr;
    std::vector<std::vector<std::uint8_t>> sent;
    bool shouldStart = true;
    bool start(Ubiquitty::Transport::ChannelObserver* suppliedObserver) override
    {
        observer = suppliedObserver;
        return shouldStart;
    }
    void send(std::span<const std::uint8_t> bytes) override { sent.emplace_back(bytes.begin(), bytes.end()); }
    void stop() override { observer = nullptr; }
    Ubiquitty::Transport::Origin origin() const override { return {}; }
    Ubiquitty::Transport::Health health() const override { return Ubiquitty::Transport::Health::Connecting; }
    std::string description() const override { return "mock bounded setup"; }
    bool boundsInitialSetupFailures() const override { return true; }
};

struct BoundedSetupFactory final : ChannelFactory {
    BoundedSetupChannel* channel = nullptr;
    unsigned requests = 0;
    bool shouldStart = true;
    void createChannel(TransportMode, Done done) override
    {
        ++requests;
        auto next = std::make_unique<BoundedSetupChannel>();
        next->shouldStart = shouldStart;
        channel = next.get();
        done(std::move(next), false);
    }
};

} // namespace

TEST(initialPassiveStreamSetupStopsAfterThreeFailuresAndManualRetryResetsIt)
{
    for (const bool shouldStart : { false, true }) {
        QueueingDispatcher dispatcher;
        BoundedSetupFactory factory;
        factory.shouldStart = shouldStart;
        CountingObserver observer;
        RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
        core.start(77, 80, 24);
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            CHECK_EQ(factory.requests, attempt);
            if (shouldStart) {
                factory.channel->observer->onClose("remote exec exited with status 1");
                dispatcher.drain();
            }
            if (attempt < 3) {
                CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
                dispatcher.fireDelayed();
            }
        }
        CHECK(core.state() == Ubiquitty::Session::State::Failed);
        CHECK(dispatcher.delayed.empty());
        core.networkPathChanged();
        core.resumeAfterSuspension(60.0);
        dispatcher.drain();
        CHECK_EQ(factory.requests, 3u);
        CHECK(core.reconnectNow());
        CHECK_EQ(factory.requests, 4u);
        if (shouldStart) {
            factory.channel->observer->onClose("remote exec exited with status 1");
            dispatcher.drain();
        }
        CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
    }
}

TEST(anEstablishedPassiveStreamKeepsItsExistingNetworkRecoveryPolicy)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    QueueingDispatcher dispatcher;
    BoundedSetupFactory factory;
    CountingObserver observer;
    RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
    core.setExpectedDaemonInstanceID(17);
    core.start(77, 80, 24);
    factory.channel->observer->onConnect();
    dispatcher.drain();
    // This fixture holds only session 77 on daemon 17. The real Greet must
    // admit that exact selection before recovery becomes an established policy.
    Sync::HostIdentity identity { .hostInstanceID = { 17, 0 }, .attachmentID = { 1, 17 }, .mayDrive = true, .verifiedKind = Sync::ClientKind::Human, .selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 77, .daemonInstanceID = 17, .socketName = "lifetime-test.sock", .hostPlatform = "fixture" } };
    Sync::HostConnection host(Sync::measuredHostPolicy(0), std::move(identity));
    REQUIRE(!factory.channel->sent.empty());
    Sync::FrameDecoder decoder;
    unsigned greetings = 0;
    for (const auto& frame : factory.channel->sent) {
        decoder.append(frame);
        while (const auto outgoing = decoder.next()) {
            REQUIRE(outgoing->kind == Sync::FrameKind::Greet);
            const auto greet = Sync::decodeGreet(outgoing->body);
            REQUIRE(greet);
            CHECK(greet->request.target == Sync::AdmissionTarget::Session);
            CHECK(greet->request.sessionID == 77 && greet->request.isAttachOnly);
            CHECK(greet->request.expectedDaemonInstanceID == 17);
            ++greetings;
        }
        host.receiveStream(frame, 1'000'000);
    }
    REQUIRE(greetings == 1);
    REQUIRE(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    REQUIRE(host.state() == Sync::ConnectionState::Attached);
    std::vector<std::uint8_t> bytes;
    unsigned welcomes = 0;
    while (host.popStreamFrame(bytes, 1'000'000)) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            CHECK(frame->kind == Sync::FrameKind::Welcome);
            welcomes += frame->kind == Sync::FrameKind::Welcome;
        }
        factory.channel->observer->onData(bytes);
        bytes.clear();
    }
    REQUIRE(welcomes == 1);
    REQUIRE(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    dispatcher.drain();
    REQUIRE(core.state() == Ubiquitty::Session::State::Connected);
    CHECK(observer.establishes == 1);
    CHECK(core.sessionID() == 77 && core.daemonInstanceID() == 17);
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        factory.channel->observer->onClose("the network path was lost");
        dispatcher.drain();
        CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
        dispatcher.fireDelayed();
        CHECK(core.state() == Ubiquitty::Session::State::Connecting);
    }
    CHECK_EQ(factory.requests, 6u);
}

TEST(aTypedPeerAuthenticationRefusalNeverRetriesOrDowngrades)
{
    for (const bool shouldStart : { false, true }) {
        QueueingDispatcher dispatcher;
        AuthenticationRefusalFactory factory;
        factory.shouldStart = shouldStart;
        CountingObserver observer;
        RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
        core.start(7, 80, 24);
        if (shouldStart) {
            REQUIRE(factory.channel && factory.channel->observer);
            factory.channel->observer->onClose("the certificate failed its pinned trust check");
            dispatcher.drain();
        }
        CHECK(core.state() == Ubiquitty::Session::State::Failed);
        CHECK_EQ(factory.requests, 1u);
        CHECK(!observer.lastDetail.empty());
        CHECK(dispatcher.delayed.empty());
        core.networkPathChanged();
        core.resumeAfterSuspension(60.0);
        CHECK(!core.reconnectNow());
        dispatcher.fireDelayed();
        dispatcher.drain();
        CHECK_EQ(factory.requests, 1u);
        CHECK_EQ(observer.establishes, 0);
    }
}

TEST(aDirectoryUsesTheFactorysControlCarrierInsteadOfRequestingASessionFallback)
{
    QueueingDispatcher dispatcher;
    FailingFactory factory;
    factory.preferredControl = TransportMode::QUIC;
    Ubiquitty::Session::SessionDirectoryObserver observer;
    Ubiquitty::Session::SessionDirectoryCore directory(&dispatcher, &factory, &observer);
    CHECK(directory.refresh());
    CHECK_EQ(factory.requests, 1u);
    CHECK(factory.requestedTransport == TransportMode::QUIC);
}

TEST(aPermanentFactoryFailurePreservesItsCauseAndDoesNotRedial)
{
    QueueingDispatcher dispatcher;
    FailingFactory factory;
    factory.failure = { .recovery = Ubiquitty::Session::ChannelFailureRecovery::Stop, .detail = "the pinned host refused this login" };
    CountingObserver observer;
    RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
    core.start(7, 80, 24);
    CHECK(core.state() == Ubiquitty::Session::State::Failed);
    CHECK(observer.lastDetail == factory.failure.detail);
    CHECK_EQ(factory.requests, 1u);
    CHECK(dispatcher.delayed.empty());
    core.networkPathChanged();
    core.resumeAfterSuspension(60.0);
    dispatcher.fireDelayed();
    dispatcher.drain();
    CHECK_EQ(factory.requests, 1u);
    CHECK_EQ(observer.exits, 0);
    CHECK_EQ(observer.establishes, 0);
}

TEST(aTransientFactoryFailureStopsAfterThreeAttemptsWithItsCause)
{
    QueueingDispatcher dispatcher;
    FailingFactory factory;
    factory.failure = { .recovery = Ubiquitty::Session::ChannelFailureRecovery::Retry, .detail = "the host accepted SSH but its bootstrap has not answered" };
    CountingObserver observer;
    RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
    core.start(7, 80, 24);
    CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
    CHECK_EQ(factory.requests, 1u);
    dispatcher.fireDelayed();
    CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
    CHECK_EQ(factory.requests, 2u);
    dispatcher.fireDelayed();
    CHECK(core.state() == Ubiquitty::Session::State::Failed);
    CHECK_EQ(factory.requests, 3u);
    CHECK(observer.lastDetail == factory.failure.detail);
    CHECK(dispatcher.delayed.empty());
    dispatcher.fireDelayed();
    CHECK_EQ(factory.requests, 3u);
    CHECK_EQ(observer.exits, 0);
    CHECK_EQ(observer.establishes, 0);
}

TEST(anAbandonedFactoryFailureCannotReplaceTheCurrentAttemptsCause)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;
    RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
    core.start(7, 80, 24);
    auto abandoned = std::move(factory.held);
    core.resumeAfterSuspension(-1.0);
    REQUIRE(factory.held);
    factory.failure = { .recovery = Ubiquitty::Session::ChannelFailureRecovery::Stop, .detail = "old attempt refused" };
    abandoned(nullptr, false);
    CHECK(core.state() == Ubiquitty::Session::State::Connecting);
    CHECK(dispatcher.delayed.empty());
    factory.failure = { .recovery = Ubiquitty::Session::ChannelFailureRecovery::Stop, .detail = "current attempt refused" };
    factory.held(nullptr, false);
    CHECK(core.state() == Ubiquitty::Session::State::Failed);
    CHECK(observer.lastDetail == "current attempt refused");
    CHECK(dispatcher.delayed.empty());
}

TEST(testAReconnectFiringAfterTheSessionIsGoneTouchesNothing)
{
    QueueingDispatcher dispatcher;
    RefusingFactory factory;
    CountingObserver observer;

    auto core = std::make_unique<RemoteSessionCore>(nullptr, &dispatcher, &factory, &observer);
    core->start(0, 80, 24);

    // No transport, so the core armed the reconnect and named itself in it.
    CHECK_EQ(factory.createCount, 1);
    CHECK(!dispatcher.delayed.empty());

    const int changesBeforeTheDestruction = observer.stateChanges;
    core.reset();

    // The deadline, reached after the pane closed. Without the weak reference
    // this reads m_reconnectEpoch through freed storage and dials from there.
    dispatcher.fireDelayed();
    dispatcher.drain();

    CHECK_EQ(factory.createCount, 1); // no second dial from a released core
    CHECK_EQ(observer.stateChanges, changesBeforeTheDestruction);
    CHECK_EQ(observer.exits, 0);
    CHECK_EQ(observer.establishes, 0);
}

TEST(testAFactoryAnswerAfterTheSessionIsGoneTouchesNothing)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;

    auto core = std::make_unique<RemoteSessionCore>(nullptr, &dispatcher, &factory, &observer);
    core->start(0, 80, 24);
    CHECK(static_cast<bool>(factory.held));

    const int changesBeforeTheDestruction = observer.stateChanges;
    core.reset();

    // The attempt's completion, answered late. Without the weak reference this
    // reads m_generation and m_stopped through freed storage.
    factory.held(nullptr, false);
    dispatcher.fireDelayed();
    dispatcher.drain();

    CHECK_EQ(observer.stateChanges, changesBeforeTheDestruction);
    CHECK_EQ(observer.exits, 0);
    CHECK_EQ(observer.establishes, 0);
}

TEST(resumingWhileBootstrappingIgnoresTheAbandonedAnswer)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;
    RemoteSessionCore core(nullptr, &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    auto abandoned = std::move(factory.held);
    core.resumeAfterSuspension(-1.0);
    CHECK(static_cast<bool>(factory.held));
    CHECK(core.state() == Ubiquitty::Session::State::Connecting);
    abandoned(nullptr, false);
    CHECK(core.state() == Ubiquitty::Session::State::Connecting);
    CHECK(dispatcher.delayed.empty());
    factory.held(nullptr, false);
    CHECK(core.state() == Ubiquitty::Session::State::Reconnecting);
    CHECK_EQ(dispatcher.delayed.size(), 1u);
}

UBIQUITTY_TEST_MAIN()
