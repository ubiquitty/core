// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_session_directory_lifetime — work handed out before the directory went
// away must not run against it afterwards.
//
// SessionDirectoryCore hands two kinds of closure to code that outlives a
// single call: the factory's completion, and the dispatcher's queue. Its
// generation counter stops a SUPERSEDED errand's callback, but it cannot stop
// one whose object is gone — reading the counter is itself a read through the
// freed object. Tearing the channel down joins the I/O thread, which ends the
// callbacks that have not started; it recalls nothing already handed over.
//
// The owner is a screen a person can leave mid-errand: the macOS picker drops
// its errand surface when the selection changes, with a terminate's timers
// still armed against it.
//
// Registered with CMake rather than written as a Host stanza on purpose: the
// sanitized driver instruments exactly what CMake registers, so a use-after-free
// here is caught by the checker and a stanza's would not be.

#include "Harness.h"

#include "Core/Session/SessionDirectoryCore.h"

#include "Core/Transport/Channel.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

/// A dispatcher that runs nothing until the case says so.
struct QueueingDispatcher final : Ubiquitty::Session::Dispatcher {
    std::vector<Ubiquitty::Session::PostedWork> queued;

    void post(Ubiquitty::Session::PostedWork work) override { queued.push_back(std::move(work)); }
    /// Timers wait apart: an errand's deadlines are not due because the loop
    /// turned.
    std::vector<Ubiquitty::Session::PostedWork> timers;
    void postDelayed(double, Ubiquitty::Session::PostedWork work) override { timers.push_back(std::move(work)); }

    /// Lets every timer out fire, as when its time has come.
    void fireTimers()
    {
        std::vector<Ubiquitty::Session::PostedWork> running;
        running.swap(timers);
        for (auto& work : running)
            std::move(work)();
    }

    /// Runs what was queued, as the platform's run loop would.
    void drain()
    {
        std::vector<Ubiquitty::Session::PostedWork> running;
        running.swap(queued);
        for (auto& work : running)
            std::move(work)();
    }
};

/// A factory that keeps the completion instead of calling it, so a case can
/// answer an errand after its owner is gone.
struct HoldingFactory final : Ubiquitty::Session::ChannelFactory {
    Done held;

    void createChannel(Ubiquitty::Session::TransportMode, Done done) override { held = std::move(done); }
};

/// A channel that connects when asked and carries nothing.
///
/// The directory's own posts are what this case needs on the queue, and it
/// makes them from the connect: a factory answered with no channel fails the
/// errand there and then, while the object is still alive, and queues nothing.
struct ConnectingChannel final : Ubiquitty::Transport::Channel {
    Ubiquitty::Transport::ChannelObserver* observer = nullptr;

    bool start(Ubiquitty::Transport::ChannelObserver* watcher) override
    {
        observer = watcher;
        if (observer)
            observer->onConnect();
        return true;
    }
    void send(std::span<const std::uint8_t>) override { }
    void stop() override { }
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const override { return {}; }
    [[nodiscard]] Ubiquitty::Transport::Health health() const override
    {
        return Ubiquitty::Transport::Health::Connected;
    }
    [[nodiscard]] std::string description() const override { return "a channel that only connects"; }
};

/// An observer that counts, so the case can say nothing reached it.
struct CountingObserver final : Ubiquitty::Session::SessionDirectoryObserver {
    int lists = 0;
    int failures = 0;
    int terminates = 0;
    Ubiquitty::Session::DirectoryFailureReport lastFailure;

    void onSessionList(const std::vector<Ubiquitty::Session::SessionRow>&) override { ++lists; }
    void onDirectoryFailed(const Ubiquitty::Session::DirectoryFailureReport& report) override
    {
        ++failures;
        lastFailure = report;
    }
    void onTerminateSettled(std::uint32_t, Ubiquitty::Session::TerminateOutcome) override { ++terminates; }
};

}

TEST(testAFactoryAnswerAfterTheDirectoryIsGoneTouchesNothing)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;

    auto directory = std::make_unique<Ubiquitty::Session::SessionDirectoryCore>(&dispatcher, &factory, &observer);
    CHECK(directory->refresh());
    CHECK(static_cast<bool>(factory.held));

    directory.reset();

    // The errand's completion, answered late. Without the weak reference this
    // reads m_generation through freed storage and goes on to use the rest.
    factory.held(nullptr, false);
    dispatcher.drain();
    // And the deadline the errand armed at its dial, come due after it.
    CHECK(!dispatcher.timers.empty());
    dispatcher.fireTimers();
    dispatcher.drain();

    CHECK_EQ(observer.lists, 0);
    CHECK_EQ(observer.failures, 0);
    CHECK_EQ(observer.terminates, 0);
}

TEST(testQueuedWorkAfterTheDirectoryIsGoneTouchesNothing)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;

    auto directory = std::make_unique<Ubiquitty::Session::SessionDirectoryCore>(&dispatcher, &factory, &observer);
    CHECK(directory->terminate(7));
    CHECK(static_cast<bool>(factory.held));

    // Answered with a channel that connects, so the directory's own marshalling
    // post reaches the queue; then the directory goes with that post still on
    // it, which is a person leaving the screen mid-errand.
    factory.held(std::make_unique<ConnectingChannel>(), false);
    const std::size_t queuedBeforeTheDestruction = dispatcher.queued.size();
    directory.reset();
    dispatcher.drain();
    dispatcher.fireTimers();
    dispatcher.drain();

    CHECK(queuedBeforeTheDestruction > 0);
    CHECK_EQ(observer.terminates, 0);
    CHECK_EQ(observer.lists, 0);
}

TEST(testDirectoryExplainsInstallationAfterNoisyStartup)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;
    Ubiquitty::Session::SessionDirectoryCore directory(&dispatcher, &factory, &observer);
    CHECK(directory.refresh());
    auto channel = std::make_unique<ConnectingChannel>();
    auto* carrier = channel.get();
    factory.held(std::move(channel), false);
    dispatcher.drain();
    const std::string text = "welcome\nUBIQUITTY-NEED-DEPLOY Linux x86_64\n";
    // The carrier exposes the string's UTF-8 storage as wire bytes.
    carrier->observer->onData({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    dispatcher.drain();
    CHECK_EQ(observer.failures, 1);
    CHECK(observer.lastFailure.failure == Ubiquitty::Session::ListingFailure::NotInstalled);
    CHECK_STR_EQ(observer.lastFailure.remotePlatform, "Linux x86_64");
    CHECK(Ubiquitty::Session::listingFailureNotice(observer.lastFailure, Ubiquitty::Session::NoticePlacement::Embedded).find("Linux x86_64") != std::string::npos);
    CHECK(Ubiquitty::Session::listingFailureNotice(observer.lastFailure, Ubiquitty::Session::NoticePlacement::Embedded).find("protocol decode fault") == std::string::npos);
    CHECK(!directory.isBusy());
}

// ssh under BatchMode cannot prompt, and says so on its standard error as
// "Permission denied". That close is a sign-in the person can make in an
// interactive shell, not a host that is down; every other close is
// unreachable, with ssh's words carried verbatim.
TEST(testACarrierClosedBySSHSignInIsTypedApartFromAnUnreachableHost)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;
    Ubiquitty::Session::SessionDirectoryCore directory(&dispatcher, &factory, &observer);

    CHECK(directory.refresh());
    auto channel = std::make_unique<ConnectingChannel>();
    auto* carrier = channel.get();
    factory.held(std::move(channel), false);
    dispatcher.drain();
    const std::string signIn = "closed: user@lab: Permission denied (publickey,password).";
    carrier->observer->onClose(signIn);
    dispatcher.drain();
    CHECK_EQ(observer.failures, 1);
    CHECK(observer.lastFailure.failure == Ubiquitty::Session::ListingFailure::SignInRequired);
    CHECK_STR_EQ(observer.lastFailure.detail, signIn);

    CHECK(directory.refresh());
    channel = std::make_unique<ConnectingChannel>();
    carrier = channel.get();
    factory.held(std::move(channel), false);
    dispatcher.drain();
    const std::string timedOut = "closed: ssh: connect to host gpu-01 port 22: Operation timed out";
    carrier->observer->onClose(timedOut);
    dispatcher.drain();
    CHECK_EQ(observer.failures, 2);
    CHECK(observer.lastFailure.failure == Ubiquitty::Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(observer.lastFailure.detail, timedOut);
}

namespace {

/// Closes one fresh errand's carrier with the reason a poll channel reports.
///
/// - Parameter reason: The channel's words, with ssh's standard error after them.
/// - Returns: What the errand reported.
Ubiquitty::Session::DirectoryFailureReport failureOfAClose(const std::string& reason)
{
    QueueingDispatcher dispatcher;
    HoldingFactory factory;
    CountingObserver observer;
    Ubiquitty::Session::SessionDirectoryCore directory(&dispatcher, &factory, &observer);
    CHECK(directory.refresh());
    auto channel = std::make_unique<ConnectingChannel>();
    auto* carrier = channel.get();
    factory.held(std::move(channel), false);
    dispatcher.drain();
    carrier->observer->onClose(reason);
    dispatcher.drain();
    CHECK_EQ(observer.failures, 1);
    return observer.lastFailure;
}

} // namespace

// What OpenSSH prints when the server's MaxAuthTries ran out: an agent full of
// other hosts' keys used every attempt before the right one. That is a sign-in
// the server ended, never a host that could not be reached, and the detail is
// ssh's account without the channel's own words for its pipe.
TEST(aServerThatEndsTheSignInAfterTooManyAttemptsIsNotAnUnreachableHost)
{
    const auto report = failureOfAClose("the connection closed: Received disconnect from 192.0.2.10 port 22:2: Too many authentication failures\r\n"
                                        "Disconnected from 192.0.2.10 port 22\r\n");
    CHECK(report.failure == Ubiquitty::Session::ListingFailure::AuthenticationAttemptsExhausted);
    CHECK_STR_EQ(report.detail, "Received disconnect from 192.0.2.10 port 22:2: Too many authentication failures\nDisconnected from 192.0.2.10 port 22");
}

// ssh says why it could not connect or sign in. A carrier that ended with
// nothing from ssh and nothing from the host ended on the remote side: the
// host was reached, and saying it could not be is the wrong remedy.
TEST(aCarrierThatEndsWithNothingSaidIsNotCalledUnreachable)
{
    const auto silent = failureOfAClose("the connection closed");
    CHECK(silent.failure == Ubiquitty::Session::ListingFailure::EndedBeforeAnswer);
    CHECK_MSG(silent.detail.empty(), "the channel's own words are not a detail");

    const auto refused = failureOfAClose("the connection closed: ssh: connect to host gpu-01 port 22: Connection refused\r\n");
    CHECK(refused.failure == Ubiquitty::Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(refused.detail, "ssh: connect to host gpu-01 port 22: Connection refused");

    const auto readError = failureOfAClose("the connection failed while receiving");
    CHECK_MSG(readError.failure == Ubiquitty::Session::ListingFailure::Unreachable, "only a clean end with nothing said is the remote side's");
    CHECK(readError.detail.empty());
}

UBIQUITTY_TEST_MAIN()
