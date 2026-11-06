// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/event_stream.cpp — the host's event stream, as a subscriber sees it.
//
// `ubq wait` and `ubq events` are subscribers: they call events.subscribe
// and are pushed a record each time something happens. Nothing here may be
// polled for, so the daemon counts every event Notify it queues, and the cases
// assert that count moves exactly when a record is made and stays put while
// nothing happens.
//
// A subscriber that reconnects names the last cursor it saw and must receive
// exactly the records after it: none twice, none skipped, and, when the
// bounded log has already dropped some, a gap record saying how many.
//
// The log is the host's, but what a subscriber is shown is its own: an agent
// or a remote client hears only the sessions it is attached to or holds a
// grant for, and a command line only with the CommandLine capability, live and
// in the backlog alike.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"
#include "NotifyRead.h"
#include "ScreenClient.h"
#include "ScreenPlaneAccess.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/EventLog.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

enum class ControlClaim {
    Held,
    Absent
};
enum class StreamStart {
    AfterCursor,
    FromLatest
};

struct EventStreamAccess {
    // A verified token principal supplied by this downstream authority-model
    // fixture, not proof of credential verification, a local binary, or TLS.
    static constexpr std::string_view tokenID = "event-stream-fixture-principal";

    struct Subscriber {
        explicit Subscriber(std::size_t clientIndex)
            : index(clientIndex)
            , screen(24, 80, Protocol::Sync::ID128 { .low = clientIndex + 1, .high = 7 }, Protocol::Sync::Role::Agent)
        {
        }
        std::size_t index;
        UbiquittyTest::ScreenClient screen;
    };
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, 24, 80);
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    // The physical connection starts ungreeted. The post-auth scope/principal
    // seam supplies authority, then one canonical Greet performs admission.
    // A no-Control caller observes its own granted pane instead: unbound
    // Control admission itself requires Control and cannot be fabricated.
    static std::unique_ptr<Subscriber> addSubscriber(Daemon& d, int* readEndOut, ControlClaim claim = ControlClaim::Held)
    {
        int fds[2] = { -1, -1 };
        if (::pipe(fds) != 0)
            return nullptr;
        ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        *readEndOut = fds[0];
        const std::size_t index = ScreenPlaneAccess::connect(d, PAL::UnixFileDescriptor(fds[1]));
        ScreenPlaneAccess::grantScope(d, index, claim == ControlClaim::Held ? Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Control } : Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe });
        ScreenPlaneAccess::setTokenID(d, index, std::string(tokenID));
        ScreenPlaneAccess::setAgentLabel(d, index, "ubq");
        Protocol::Sync::AdmissionRequest request { .target = Protocol::Sync::AdmissionTarget::Control,
            .expectedDaemonInstanceID = ScreenPlaneAccess::daemonInstanceID(d),
            .agentLabel = "ubq" };
        if (claim == ControlClaim::Absent) {
            auto& session = ScreenPlaneAccess::makeSession(d, 24, 80);
            ScreenPlaneAccess::grantAgentSession(d, session, index);
            request.target = Protocol::Sync::AdmissionTarget::Session;
            request.sessionID = session.id;
            request.isAttachOnly = true;
        }
        auto subscriber = std::make_unique<Subscriber>(index);
        CHECK(subscriber->screen.start(request));
        ScreenPlaneAccess::receive(d, index, subscriber->screen.takeStream(UbiquittyTest::screenClientNow()));
        (void)received(d, subscriber, *readEndOut); // Welcome, not an event.
        const auto welcome = subscriber->screen.welcome();
        CHECK(welcome && welcome->admission == Protocol::Sync::AttachmentAdmission::Granted);
        if (welcome) {
            CHECK(welcome->selection.target == request.target
                && welcome->selection.sessionID == request.sessionID
                && welcome->selection.daemonInstanceID == request.expectedDaemonInstanceID);
            if (claim == ControlClaim::Held) {
                CHECK(welcome->selection.workspaceID == 0);
                CHECK(subscriber->screen.connection().boundPanes().empty());
            }
        }
        return subscriber;
    }

    /// The standing grant an agent holds on a session it created or was
    /// allowed, keyed as the daemon keys this seam's subscribers.
    static void grant(Daemon& d, std::uint32_t sessionID)
    {
        if (Daemon::Session* s = d.findSession(sessionID)) {
            (void)s->addSessionGrant("token:" + std::string(tokenID));
        }
    }

    /// Withdraws the standing grant as `consent.revoke` does for a session
    /// grant: the session no longer names the subscriber's identity.
    static void revoke(Daemon& d, std::uint32_t sessionID)
    {
        if (Daemon::Session* s = d.findSession(sessionID)) {
            s->agentSessionGrantIdentities.clear();
        }
    }

    static void subscribe(Daemon& d, const std::unique_ptr<Subscriber>& subscriber, std::uint64_t since, StreamStart start)
    {
        CHECK(subscriber);
        if (!subscriber)
            return;
        const auto request = Protocol::Control::eventSubscriptionRequest({ .after = start == StreamStart::FromLatest ? 0 : since, .isFromLatest = start == StreamStart::FromLatest }, "events.fixture");
        CHECK(subscriber->screen.call(request));
        ScreenPlaneAccess::receive(d, subscriber->index, subscriber->screen.takeStream(UbiquittyTest::screenClientNow()));
        d.serviceSyncConnection(subscriber->index, UbiquittyTest::screenClientNow());
    }

    // Every canonical event batch received since the previous pass.
    static std::vector<Ubiquitty::Session::Event> received(Daemon& d, const std::unique_ptr<Subscriber>& subscriber, int readEnd)
    {
        CHECK(subscriber);
        if (!subscriber)
            return {};
        const std::size_t index = subscriber->index;
        const auto upstream = subscriber->screen.takeStream(UbiquittyTest::screenClientNow());
        if (!upstream.empty())
            ScreenPlaneAccess::receive(d, index, upstream);
        d.serviceSyncConnection(index, UbiquittyTest::screenClientNow());
        d.flushClient(index);
        std::vector<std::uint8_t> bytes;
        std::uint8_t chunk[65536];
        for (;;) {
            const ssize_t n = ::read(readEnd, chunk, sizeof chunk);
            if (n <= 0)
                break;
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        Daemon::Client& c = d.m_clients[index];
        if (c.outputConsumed < c.outputBuffer.size())
            bytes.insert(bytes.end(), c.outputBuffer.begin() + static_cast<std::ptrdiff_t>(c.outputConsumed), c.outputBuffer.end());
        c.clearPendingOutput();
        std::vector<Ubiquitty::Session::Event> events;
        subscriber->screen.read(bytes, UbiquittyTest::screenClientNow());
        for (const auto& notification : subscriber->screen.takeNotifies()) {
            if (auto batch = UbiquittyTest::eventsOf(notification)) {
                for (auto& event : *batch)
                    events.push_back(std::move(event));
            }
        }
        return events;
    }

    static void screenAccessIsScopedAndRetained()
    {
        namespace Sync = Protocol::Sync;
        using Screen = ScreenPlaneAccess;
        using Kind = Ubiquitty::Session::EventKind;
        Daemon daemon;
        auto& pane = Screen::makeSession(daemon, 24, 80);
        auto& unrelated = Screen::makeSession(daemon, 24, 80);
        UbiquittyTest::feed(*pane.term, "private screen text");
        Screen::publish(daemon, pane, Runtime::FrameClass::Structural);
        const auto human = [&](std::uint32_t sessionID) {
            const auto index = Screen::attach(daemon, sessionID);
            Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Drive });
            Screen::markInteractiveUser(daemon, index);
            Screen::setTokenID(daemon, index, "human-" + std::to_string(index));
            return index;
        };
        const auto ownerIndex = human(pane.id);
        UbiquittyTest::ScreenClient owner(24, 80, Sync::ID128 { 101, 1 });
        Screen::greet(daemon, ownerIndex, owner);
        const auto otherIndex = human(unrelated.id);
        UbiquittyTest::ScreenClient other(24, 80, Sync::ID128 { 102, 1 });
        Screen::greet(daemon, otherIndex, other);
        const auto agentIndex = Screen::attach(daemon, pane.id);
        Screen::grantScope(daemon, agentIndex, { Sync::Scope::Observe, Sync::Scope::RegionRead });
        Screen::setTokenID(daemon, agentIndex, "screen-reader");
        Screen::setAgentLabel(daemon, agentIndex, "claimed reader");
        Screen::grantAgentSession(daemon, pane, agentIndex);
        UbiquittyTest::ScreenClient agent(24, 80, Sync::ID128 { 103, 1 }, Sync::Role::Agent);
        Screen::greet(daemon, agentIndex, agent);
        CHECK_MSG(Screen::eventCount(daemon, Kind::ScreenRead) == 0, "admission alone is not a screen read");
        const auto capture = [&](std::string_view parameters) {
            const auto request = Protocol::Sync::decodeCallRequest(std::string(R"({"jsonrpc":"2.0","id":"capture","method":"pane.capture","params":)") + std::string(parameters) + "}");
            return daemon.serveScreenCall(daemon.m_clients[agentIndex], request);
        };
        CHECK(capture(R"({"pane":1,"source":"screen"})").find("result"));
        CHECK(Screen::eventCount(daemon, Kind::ScreenRead) == 1);
        CHECK(capture(R"({"pane":1,"source":"rows","start":99999,"end":99999})").find("error"));
        CHECK(Screen::eventCount(daemon, Kind::ScreenRead) == 1);
        Screen::grantScope(daemon, agentIndex, { Sync::Scope::Observe });
        CHECK(capture(R"({"pane":1,"source":"tail","lines":2})").find("error"));
        CHECK_MSG(Screen::eventCount(daemon, Kind::ScreenRead) == 1, "a denied read records no disclosure");
        const auto reads = [&](std::size_t index, UbiquittyTest::ScreenClient& client) {
            Screen::deliver(daemon, index, client);
            std::vector<Ubiquitty::Session::Event> result;
            for (const auto& notification : client.takeNotifies())
                if (const auto events = UbiquittyTest::eventsOf(notification))
                    for (const auto& event : *events)
                        if (event.kind == Kind::ScreenRead)
                            result.push_back(event);
            return result;
        };
        auto events = reads(ownerIndex, owner);
        CHECK_MSG(events.size() == 1, "an attached human receives activity without Control scope");
        if (!events.empty()) {
            CHECK(events[0].sessionID == pane.id);
            CHECK(events[0].text == "claimed reader" && events[0].count > 0);
            CHECK(events[0].detail.empty());
        }
        CHECK_MSG(reads(otherIndex, other).empty(), "another pane cannot see this pane's readers");
        CHECK_MSG(reads(agentIndex, agent).empty(), "automation cannot read the privacy feed");
        Screen::endReadPass(daemon);
        Screen::publish(daemon, pane, Runtime::FrameClass::Structural);
        agent.setView(Sync::ViewState::Live);
        Screen::deliver(daemon, agentIndex, agent);
        CHECK_MSG(Screen::eventCount(daemon, Kind::ScreenRead) == 2, "streamed screens used by screenshots record access too");
        Screen::deliver(daemon, agentIndex, agent);
        CHECK_MSG(Screen::eventCount(daemon, Kind::ScreenRead) == 2, "an unchanged stream does not manufacture reads");
        Screen::drop(daemon, agentIndex);
        CHECK(reads(ownerIndex, owner).size() == 1);
        const auto returningIndex = human(pane.id);
        UbiquittyTest::ScreenClient returning(24, 80, Sync::ID128 { 104, 1 });
        Screen::greet(daemon, returningIndex, returning);
        CHECK_MSG(reads(returningIndex, returning).size() == 2, "retained activity survives the reader disconnecting and the human reconnecting");
        CHECK(reads(returningIndex, returning).empty());
    }

    static void publish(Daemon& d, Daemon::Session& s) { d.emitAgentEvents(s); }
    static std::uint64_t deliveries(const Daemon& d) { return d.m_eventDeliveries; }
    static void shrinkLog(Daemon& d, std::size_t capacity) { d.m_events = Ubiquitty::Session::EventLog(capacity); }
    static void bell(Daemon& d, std::uint32_t sessionID)
    {
        Ubiquitty::Session::Event rang;
        rang.kind = Ubiquitty::Session::EventKind::Bell;
        rang.sessionID = sessionID;
        d.recordEvent(std::move(rang));
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::EventStreamAccess;
using Ubiquitty::Session::EventKind;
using Ubiquitty::Session::EventProgramState;

int main()
{
    EventStreamAccess::screenAccessIsScopedAndRetained();
    // ---- 1. A finished command reaches the subscriber once, with its exit
    // status, and nothing is delivered while nothing happens.
    {
        Daemon d;
        auto& s = EventStreamAccess::addSession(d, 1);
        EventStreamAccess::grant(d, 1);
        int readEnd = -1;
        const auto subscriber = EventStreamAccess::addSubscriber(d, &readEnd);
        EventStreamAccess::subscribe(d, subscriber, 0, Ubiquitty::Host::StreamStart::FromLatest);
        auto events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 1 && events[0].kind == EventKind::Gap && events[0].count == 0);
        const std::uint64_t afterSubscribe = EventStreamAccess::deliveries(d);

        // A quiet session publishes and publishes again; no record, no frame.
        EventStreamAccess::publish(d, s);
        EventStreamAccess::publish(d, s);
        CHECK(EventStreamAccess::deliveries(d) == afterSubscribe);
        CHECK(EventStreamAccess::received(d, subscriber, readEnd).empty());

        UbiquittyTest::feed(*s.term, "\033]133;C\007make\r\n\033]133;D;3\007");
        EventStreamAccess::publish(d, s);
        events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 2);
        if (events.size() == 2) {
            CHECK(events[0].kind == EventKind::CommandStarted);
            CHECK(events[1].kind == EventKind::CommandFinished);
            CHECK(events[1].exitStatus == 3);
            CHECK(events[1].sessionID == 1);
        }
        CHECK(EventStreamAccess::deliveries(d) == afterSubscribe + 2);

        // The same start and completion are not reported twice by the next publish.
        EventStreamAccess::publish(d, s);
        CHECK(EventStreamAccess::deliveries(d) == afterSubscribe + 2);
    }

    // ---- 2. A subscriber resuming from a cursor gets exactly what followed it.
    {
        Daemon d;
        EventStreamAccess::addSession(d, 1);
        EventStreamAccess::grant(d, 1);
        int firstEnd = -1;
        const auto first = EventStreamAccess::addSubscriber(d, &firstEnd);
        EventStreamAccess::subscribe(d, first, 0, Ubiquitty::Host::StreamStart::AfterCursor);
        for (int i = 0; i < 3; ++i)
            EventStreamAccess::bell(d, 1);
        const auto seen = EventStreamAccess::received(d, first, firstEnd);
        CHECK(seen.size() == 3);
        const std::uint64_t saved = seen.size() >= 2 ? seen[1].cursor : 0;

        // Two more records while the second subscriber was away.
        EventStreamAccess::bell(d, 1);
        EventStreamAccess::bell(d, 1);
        int secondEnd = -1;
        const auto second = EventStreamAccess::addSubscriber(d, &secondEnd);
        EventStreamAccess::subscribe(d, second, saved, Ubiquitty::Host::StreamStart::AfterCursor);
        const auto resumed = EventStreamAccess::received(d, second, secondEnd);
        CHECK(resumed.size() == 3);
        for (std::size_t i = 0; i < resumed.size(); ++i) {
            CHECK(resumed[i].kind == EventKind::Bell);
            CHECK(resumed[i].cursor == saved + 1 + i);
        }
    }

    // ---- 3. A subscriber the bounded log outran is told how many it lost.
    {
        Daemon d;
        EventStreamAccess::addSession(d, 1);
        EventStreamAccess::grant(d, 1);
        EventStreamAccess::shrinkLog(d, 2);
        for (int i = 0; i < 5; ++i)
            EventStreamAccess::bell(d, 1);
        int readEnd = -1;
        const auto subscriber = EventStreamAccess::addSubscriber(d, &readEnd);
        EventStreamAccess::subscribe(d, subscriber, 1, Ubiquitty::Host::StreamStart::AfterCursor);
        const auto events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 3);
        if (events.size() == 3) {
            CHECK(events[0].kind == EventKind::Gap);
            CHECK(events[0].count == 2); // cursors 2 and 3 are gone
            CHECK(events[1].cursor == 4 && events[2].cursor == 5);
        }
    }

    // ---- 4. An OSC 7501 report is a status event carrying its state and
    // message; clearing it reports state none.
    {
        Daemon d;
        auto& s = EventStreamAccess::addSession(d, 1);
        EventStreamAccess::grant(d, 1);
        int readEnd = -1;
        const auto subscriber = EventStreamAccess::addSubscriber(d, &readEnd);
        EventStreamAccess::subscribe(d, subscriber, 0, Ubiquitty::Host::StreamStart::FromLatest);
        EventStreamAccess::received(d, subscriber, readEnd);

        UbiquittyTest::feed(*s.term, "\033]7501;id=build:state=blocked:kind=question:msg=Q29udGludWU/\033\\");
        EventStreamAccess::publish(d, s);
        auto events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 1);
        if (!events.empty()) {
            CHECK(events[0].kind == EventKind::ProgramStatus);
            CHECK(events[0].state == EventProgramState::Blocked);
            CHECK(events[0].text == "Continue?");
            CHECK(events[0].detail == "build");
        }
        EventStreamAccess::publish(d, s);
        CHECK(EventStreamAccess::received(d, subscriber, readEnd).empty());

        UbiquittyTest::feed(*s.term, "\033]7501;id=build:state=clear\033\\");
        EventStreamAccess::publish(d, s);
        events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 1 && events[0].state == EventProgramState::None);
    }

    // ---- 5. An admitted observer whose token holds no Control scope receives
    // a correlated refusal, not an event subscription or any records.
    {
        Daemon d;
        int readEnd = -1;
        const auto outsider = EventStreamAccess::addSubscriber(d, &readEnd, Ubiquitty::Host::ControlClaim::Absent);
        EventStreamAccess::subscribe(d, outsider, 0, Ubiquitty::Host::StreamStart::AfterCursor);
        EventStreamAccess::bell(d, 1);
        CHECK(EventStreamAccess::received(d, outsider, readEnd).empty());
        const auto answers = outsider->screen.takeResults();
        CHECK(answers.size() == 1);
        if (answers.size() == 1) {
            bool isParsed = false;
            const auto answer = Ubiquitty::Codec::JSON::parse(answers.front(), isParsed);
            const auto* error = isParsed ? answer.find("error") : nullptr;
            CHECK(isParsed && answer.stringOr("id") == "events.fixture" && error
                && error->numberOr("code") == std::to_underlying(Ubiquitty::Protocol::Control::ErrorCode::Forbidden));
        }
        CHECK(EventStreamAccess::deliveries(d) == 0);
    }

    // ---- 6. An agent hears only the sessions it holds a grant for, with the
    // command lines of the sessions it hears, in the backlog and live.
    {
        Daemon d;
        EventStreamAccess::addSession(d, 1);
        auto& theirs = EventStreamAccess::addSession(d, 3);
        EventStreamAccess::grant(d, 1);
        UbiquittyTest::feed(*theirs.term, "\033]633;E;mysql -psecret\007\033]133;C\007\r\n\033]133;D;0\007");
        EventStreamAccess::publish(d, theirs);
        EventStreamAccess::bell(d, 1);

        int strangerEnd = -1;
        const auto stranger = EventStreamAccess::addSubscriber(d, &strangerEnd);
        EventStreamAccess::subscribe(d, stranger, 0, Ubiquitty::Host::StreamStart::AfterCursor);
        auto backlog = EventStreamAccess::received(d, stranger, strangerEnd);
        CHECK_MSG(std::ranges::none_of(backlog, [](const auto& event) { return event.sessionID == 3; }), "an ungranted agent was replayed another session's events");
        CHECK_MSG(std::ranges::any_of(backlog, [](const auto& event) { return event.sessionID == 1 && event.kind == EventKind::Bell; }), "an agent was not replayed its own session's bell");

        UbiquittyTest::feed(*theirs.term, "\033]633;E;mysql -psecret\007\033]133;C\007\r\n\033]133;D;1\007");
        EventStreamAccess::publish(d, theirs);
        CHECK_MSG(EventStreamAccess::received(d, stranger, strangerEnd).empty(), "an ungranted agent was pushed another session's command");

        EventStreamAccess::grant(d, 3);
        int grantedEnd = -1;
        const auto granted = EventStreamAccess::addSubscriber(d, &grantedEnd);
        EventStreamAccess::subscribe(d, granted, 0, Ubiquitty::Host::StreamStart::AfterCursor);
        const auto backlogOfGranted = EventStreamAccess::received(d, granted, grantedEnd);
        const auto finished = std::ranges::find_if(backlogOfGranted, [](const auto& event) { return event.kind == EventKind::CommandFinished; });
        CHECK_MSG(finished != backlogOfGranted.end(), "a granted agent was not replayed the session's command");
        if (finished != backlogOfGranted.end())
            CHECK_MSG(finished->text == "mysql -psecret", "a granted agent was replayed a command without its line");

        int claimedEnd = -1;
        const auto claimed = EventStreamAccess::addSubscriber(d, &claimedEnd);
        EventStreamAccess::subscribe(d, claimed, 0, Ubiquitty::Host::StreamStart::FromLatest);
        EventStreamAccess::received(d, claimed, claimedEnd);
        UbiquittyTest::feed(*theirs.term, "\033]633;E;make\007\033]133;C\007\r\n\033]133;D;2\007");
        EventStreamAccess::publish(d, theirs);
        const auto live = EventStreamAccess::received(d, claimed, claimedEnd);
        CHECK(live.size() == 2 && live.back().kind == EventKind::CommandFinished && live.back().text == "make");
    }

    // ---- 7. A command's start is recorded in the order the shell wrote it,
    // so the end that follows it is told from an end written before it, even
    // when both arrive in one batch.
    {
        Daemon d;
        auto& s = EventStreamAccess::addSession(d, 1);
        EventStreamAccess::grant(d, 1);
        int readEnd = -1;
        const auto subscriber = EventStreamAccess::addSubscriber(d, &readEnd);
        EventStreamAccess::subscribe(d, subscriber, 0, Ubiquitty::Host::StreamStart::FromLatest);
        EventStreamAccess::received(d, subscriber, readEnd);
        UbiquittyTest::feed(*s.term, "\033]133;D;7\007\033]133;C\007make\r\n\033]133;D;5\007");
        EventStreamAccess::publish(d, s);
        const auto events = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(events.size() == 3);
        if (events.size() == 3) {
            CHECK(events[0].kind == EventKind::CommandFinished && events[0].exitStatus == 7);
            CHECK(events[1].kind == EventKind::CommandStarted);
            CHECK(events[2].kind == EventKind::CommandFinished && events[2].exitStatus == 5);
        }
        UbiquittyTest::feed(*s.term, "\033]133;C\007");
        EventStreamAccess::publish(d, s);
        const auto running = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK(running.size() == 1 && running[0].kind == EventKind::CommandStarted);
        EventStreamAccess::publish(d, s);
        CHECK(EventStreamAccess::received(d, subscriber, readEnd).empty());
    }

    // ---- 8. Each event asks whether the subscriber could watch its session
    // now, so a grant revoked after the subscription began stops that
    // session's events for it and leaves the sessions it still holds.
    {
        Daemon d;
        EventStreamAccess::addSession(d, 1);
        EventStreamAccess::addSession(d, 2);
        EventStreamAccess::grant(d, 1);
        EventStreamAccess::grant(d, 2);
        int readEnd = -1;
        const auto subscriber = EventStreamAccess::addSubscriber(d, &readEnd);
        EventStreamAccess::subscribe(d, subscriber, 0, Ubiquitty::Host::StreamStart::FromLatest);
        EventStreamAccess::received(d, subscriber, readEnd);
        EventStreamAccess::bell(d, 1);
        EventStreamAccess::bell(d, 2);
        const auto before = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK_MSG(before.size() == 2, "a granted subscriber was not told both sessions' bells");
        EventStreamAccess::revoke(d, 1);
        const auto delivered = EventStreamAccess::deliveries(d);
        EventStreamAccess::bell(d, 1);
        EventStreamAccess::bell(d, 2);
        const auto after = EventStreamAccess::received(d, subscriber, readEnd);
        CHECK_MSG(after.size() == 1 && after.front().sessionID == 2, "a revoked grant still delivered its session's events");
        CHECK_MSG(EventStreamAccess::deliveries(d) == delivered + 1, "a revoked session's event was queued");
    }

    if (failures == 0)
        std::printf("event_stream: a subscriber is pushed each record once, with nothing delivered while "
                    "nothing happens; a resumed subscriber gets exactly what followed its cursor, and one "
                    "the log outran is told how many it lost, and an agent hears only the sessions it "
                    "holds a grant for, which a revoked grant stops at once\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
