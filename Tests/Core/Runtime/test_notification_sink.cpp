// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_notification_sink.cpp — the session's own ShellClient, for a frontend
// that installs none.
//
// OSC 9 (iTerm), OSC 99 (kitty) and OSC 777;notify (urxvt) reach a frontend
// through `ShellClient::postNotification`, which the parser calls inline, on
// the ingest thread, with the model lock held. The classic macOS frontend
// installs a client that hops each call to its main queue; a Swift frontend
// cannot install one at all, because cxx-interop does not let a Swift type
// subclass a C++ class with virtual methods. With no client
// `OSC::dispatchNotification` returns at its first line, so a frontend that
// installs none has every notification in all three dialects parsed and
// dropped.
//
// What these pin is the contract a frontend reads the queue under: the three
// dialects landing as one resolved title/body pair, the capability gate still
// deciding whether anything is recorded at all, the bound and the coalescing
// that keep a runaway program from filling it, the event that says to look,
// and the refusal that keeps this from displacing a frontend which installed
// its own client.

#include "Core/Runtime/TerminalEvents.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Terminal.h"
#include "PAL/Capability.h"
#include "PAL/TerminalClients.h"

#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Runtime::EventKind;
using Session = Ubiquitty::Runtime::TerminalSession;
using Notification = Session::Notification;

namespace {

/// Feeds a string to the session as though a program had written it.
///
/// - Parameters:
///   - session: The session to feed.
///   - bytes: What the program wrote.
void feed(Session* session, const std::string& bytes)
{
    session->feedBytes(Ubiquitty::Runtime::ByteSpan(bytes.data(), bytes.size()));
}

/// Everything the queue holds, drained.
///
/// - Parameter session: The session to drain.
/// - Returns: The notifications, oldest first.
std::vector<Notification> drain(Session* session)
{
    std::vector<Notification> taken;
    for (;;) {
        Notification notification = session->takeNotification();
        if (!notification.isPresent)
            break;
        taken.push_back(std::move(notification));
    }
    return taken;
}

/// A client that records, standing in for a frontend that installs its own.
struct RecordingShellClient final : Ubiquitty::ShellClient {
    int calls = 0;
    std::string title;
    std::string body;
    void postNotification(std::string_view postedTitle, std::string_view postedBody) override
    {
        ++calls;
        title = std::string(postedTitle);
        body = std::string(postedBody);
    }
};

/// Answers one capability with a fixed decision and allows the rest.
class NotificationPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    explicit NotificationPolicy(Ubiquitty::CapabilityDecision decision)
        : m_decision(decision)
    {
    }

    Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        if (capability == Ubiquitty::Capability::DesktopNotification) {
            ++m_checks;
            return m_decision;
        }
        return Ubiquitty::CapabilityDecision::Allow;
    }

    /// How many times the notification capability was asked about.
    int checks() const { return m_checks; }

private:
    Ubiquitty::CapabilityDecision m_decision;
    int m_checks { 0 };
};

} // namespace

/// All three dialects reach the queue, as one resolved title/body pair.
///
/// The dialects disagree about which field a bare message is: OSC 9 and kitty's
/// OSC 99 carry a body and no title, urxvt's OSC 777 carries a title first. A
/// queue that handed those through unresolved would make every backend rederive
/// the promotion, and the arm that got it wrong would show a blank heading over
/// the text.
TEST(theNotificationQueueCarriesEveryDialectAsAResolvedPair)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installNotificationQueue());

    // OSC 9 (iTerm): body only.
    feed(session.get(), "\x1b]9;Build finished\x07");
    // OSC 99 (kitty): one sequence is a title; a title chunk and a body
    // chunk under one name are one notification, which carries the name.
    feed(session.get(), "\x1b]99;;Hello kitty\x1b\\");
    feed(session.get(), "\x1b]99;i=n1:d=0;Alert\x1b\\");
    feed(session.get(), "\x1b]99;i=n1:p=body:o=unfocused;Body text\x1b\\");
    // OSC 777 (urxvt): title, then body.
    feed(session.get(), "\x1b]777;notify;Title here;Body here\x1b\\");

    const std::vector<Notification> taken = drain(session.get());
    // Counted before it is indexed: a queue that kept one of the four would
    // otherwise trap on the subscript, and a case that crashes says only that
    // it crashed.
    CHECK_EQ(taken.size(), std::size_t { 4 });
    if (taken.size() == 4) {
        // Body-only promotes into the heading, so the banner reads as the text
        // rather than as a subtitle under an empty title.
        CHECK(taken[0].title == "Build finished");
        CHECK(taken[0].body.empty());
        CHECK(taken[1].title == "Hello kitty");
        CHECK(taken[1].body.empty());
        CHECK(taken[1].identifier.empty());
        CHECK(taken[1].occasion == Ubiquitty::NotificationOccasion::Always);
        CHECK(taken[2].title == "Alert");
        CHECK(taken[2].body == "Body text");
        CHECK(taken[2].identifier == "n1");
        CHECK(taken[2].occasion == Ubiquitty::NotificationOccasion::Unfocused);
        CHECK(taken[3].title == "Title here");
        CHECK(taken[3].body == "Body here");
    }

    // Drained is drained: a frontend that asks again gets the absent answer
    // rather than the last banner a second time.
    CHECK(!session->takeNotification().isPresent);
    CHECK(!session->hasPendingNotification());
}

/// ConEmu's progress overload of OSC 9 is not a notification.
///
/// `9;4;<state>;<value>` is a taskbar band, and a queue that recorded it would
/// banner the reader once per percent of a download.
TEST(theNotificationQueueLeavesTheProgressOverloadAlone)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installNotificationQueue());

    feed(session.get(), "\x1b]9;4;1;50\x07");
    CHECK(!session->hasPendingNotification());

    // And the empty payload, which names nothing to show.
    feed(session.get(), "\x1b]9;\x07");
    CHECK(!session->hasPendingNotification());
}

/// The host's capability policy still decides whether anything is recorded.
///
/// The sink is a delivery route, not a permission: the gate that already stood
/// in `OSC::dispatchNotification` runs before the queue is touched, so a denied
/// stream reaches the desktop through this path no more than through the
/// classic one.
TEST(theNotificationQueueObeysTheDesktopNotificationCapability)
{
    for (const auto decision : { Ubiquitty::CapabilityDecision::Deny, Ubiquitty::CapabilityDecision::Prompt }) {
        NotificationPolicy policy(decision);
        const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
        REQUIRE(session != nullptr);
        session->start(24, 80);
        CHECK(session->installNotificationQueue());
        Ubiquitty::Terminal* terminal = session->terminal();
        REQUIRE(terminal != nullptr);
        terminal->capabilityPolicy = &policy;

        // Each dialect, so none of the three reaches the desktop on a route
        // the other two are refused on.
        feed(session.get(), "\x1b]9;Build finished\x07");
        feed(session.get(), "\x1b]99;i=1:p=body;Body text\x1b\\");
        feed(session.get(), "\x1b]777;notify;Title here;Body here\x1b\\");

        CHECK_EQ(policy.checks(), 3);
        CHECK(!session->hasPendingNotification());
        terminal->capabilityPolicy = nullptr;
    }

    // And the other direction, which is what keeps the case above from passing
    // on a path that records nothing at all.
    NotificationPolicy allowing(Ubiquitty::CapabilityDecision::Allow);
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installNotificationQueue());
    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->capabilityPolicy = &allowing;
    feed(session.get(), "\x1b]9;Build finished\x07");
    CHECK_EQ(allowing.checks(), 1);
    CHECK_EQ(drain(session.get()).size(), std::size_t { 1 });
    terminal->capabilityPolicy = nullptr;
}

/// The drain raises the event, and only when a banner arrived.
///
/// A notification moves no cell, so without the bit it sits in the queue until
/// the program happens to print — which for a build that finishes and waits is
/// never.
TEST(theNotificationQueueRaisesItsEventOncePerArrival)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installNotificationQueue());

    const auto bit = 1u << static_cast<unsigned>(EventKind::NotificationPosted);

    feed(session.get(), "hello");
    CHECK((session->takeEventFlags() & bit) == 0);

    feed(session.get(), "\x1b]9;Build finished\x07");
    CHECK((session->takeEventFlags() & bit) != 0);

    // Taken once. A second read without a second banner must not say one is
    // waiting, or a frontend loops on an empty queue every drain.
    feed(session.get(), "hello");
    CHECK((session->takeEventFlags() & bit) == 0);

    CHECK_EQ(drain(session.get()).size(), std::size_t { 1 });
}

/// A program cannot grow the queue without bound, and cannot fill it with one
/// banner repeated.
///
/// Anything the terminal displays can ask for a banner: a file catted to the
/// screen, a compromised remote. The bound caps what memory a runaway costs,
/// and the coalescing caps what the reader has to dismiss — a loop emitting one
/// sequence is one banner, not eight copies of it.
TEST(theNotificationQueueRefusesToGrowPastItsBound)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installNotificationQueue());

    std::string repeated;
    for (int index = 0; index < 64; ++index)
        repeated += "\x1b]9;same text\x07";
    feed(session.get(), repeated);
    CHECK_EQ(drain(session.get()).size(), std::size_t { 1 });

    // Distinct ones do queue, up to the ceiling; the overrun is dropped.
    std::string flood;
    for (int index = 0; index < 64; ++index)
        flood += "\x1b]9;text " + std::to_string(index) + "\x07";
    feed(session.get(), flood);

    const std::vector<Notification> taken = drain(session.get());
    CHECK_EQ(taken.size(), Session::notificationQueueLimit);
    // The ones kept are the first ones: dropping the oldest would reorder what
    // survives against what the program wrote.
    if (taken.size() == Session::notificationQueueLimit)
        CHECK(taken[0].title == "text 0");
}

/// The queue never displaces a frontend that installed its own client.
///
/// A frontend can install one and answer the parser through it — for
/// the working directory as well as the banner. A session that replaced it
/// would leave that window silently unable to report either, which is a worse
/// failure than the one this exists to fix.
TEST(theNotificationQueueYieldsToAFrontendThatInstalledItsOwnClient)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);

    RecordingShellClient client;
    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->shellClient = &client;
    CHECK(!session->installNotificationQueue());

    feed(session.get(), "\x1b]777;notify;Title here;Body here\x1b\\");
    // The frontend's client got it, unresolved — the promotion is the sink's,
    // and a frontend with its own client keeps the pair the parser produced.
    CHECK_EQ(client.calls, 1);
    CHECK(client.title == "Title here");
    CHECK(client.body == "Body here");
    CHECK(!session->hasPendingNotification());

    terminal->shellClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
