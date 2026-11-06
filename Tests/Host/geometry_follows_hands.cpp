// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/geometry_follows_hands.cpp — a pane's size follows the hands
// (Docs/sync.md §6.10), as the program reads it with TIOCGWINSZ.
//
// Two Sync clients with different windows show one session on a real PTY:
//
//   1. before anyone types, the newest window applies;
//   2. the client that types owns the size; the other's pointer motion does
//      not take it, and its typing does;
//   3. `pane.pinGeometry` from a driver freezes the size whoever types, and
//      with no size lets the owner's apply again; a caller without the Drive
//      scope is answered forbidden and changes nothing;
//   4. two clients typing in turn every 10 ms resize the program at most
//      once per coalesce window;
//   5. a client holding an automation token, which declares itself a person,
//      stands as the agent the daemon verifies it is: its typing never takes
//      the size;
//   6. `pane.overrideGeometry` from that agent sets the size once: its own
//      typing does not end it and a driver's does; a caller without the
//      Drive scope is answered forbidden, and a size of zero is refused;
//   7. a person attaching alone to a session takes it to the grid its HELLO
//      names, and an agent attaching alone does not: an agent sizes a
//      session only by `pane.overrideGeometry`. A pin stands through a
//      person's attach, and an override is the arbiter's to end, not the
//      HELLO's.
//
// The seam supplies adapter-verified token scope/kind facts; this is a geometry
// test, not token-store verification. Agent admission still needs exact prior
// owner consent and goes through the production canonical greeting.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include <sys/ioctl.h>
#include <sys/socket.h>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// The size the program reads.
struct WindowSize {
    int rows = 0;
    int columns = 0;

    friend bool operator==(const WindowSize&, const WindowSize&) = default;
};

WindowSize programSize(int fileDescriptor)
{
    winsize size {};
    if (::ioctl(fileDescriptor, TIOCGWINSZ, &size) != 0)
        return {};
    return { .rows = size.ws_row, .columns = size.ws_col };
}

/// Who attaches alone to a session.
enum class Attaching : std::uint8_t {
    /// A client holding a token minted for a person.
    Person,
    /// A client holding an automation token, let in without asking.
    Agent,
    /// A person, to a session whose size is pinned.
    PersonToPinnedSession,
    /// A person, to a session an agent set the size of.
    PersonToOverriddenSession,
};

std::string describe(WindowSize size)
{
    return std::to_string(size.columns) + "x" + std::to_string(size.rows);
}

} // namespace

int main()
{
    // As the daemon's own process does: a write to a carrier whose reader has
    // gone is an error to handle, not a signal that ends the fixture.
    ::signal(SIGPIPE, SIG_IGN);
    Daemon d;
    const std::array<std::string, 3> command { "/bin/sh", "-c", "sleep 60" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    const int fileDescriptor = session->stream->fileDescriptor();
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    // Each client's carrier holds what the daemon flushes outside a service
    // pass, the greet-time seed among them, for deliver to hand over.
    std::array<Ubiquitty::HostTest::FullCarrier, 4> carriers;
    for (const auto& carrier : carriers)
        CHECK_MSG(carrier.isReady(), "premise: a carrier was not made");
    const std::size_t aIndex = Screen::attach(d, session->id, carriers[0].takeDaemonEnd());
    Screen::grantScope(d, aIndex, driving);
    Screen::setTokenID(d, aIndex, "a");
    Screen::markInteractiveUser(d, aIndex);
    const std::size_t bIndex = Screen::attach(d, session->id, carriers[1].takeDaemonEnd());
    Screen::grantScope(d, bIndex, driving);
    Screen::setTokenID(d, bIndex, "b");
    Screen::markInteractiveUser(d, bIndex);
    const std::size_t eIndex = Screen::attach(d, session->id, carriers[2].takeDaemonEnd());
    Screen::grantScope(d, eIndex, driving);
    Screen::setTokenID(d, eIndex, "e");
    const std::size_t cIndex = Screen::attach(d, session->id, carriers[3].takeDaemonEnd());
    Screen::grantScope(d, cIndex, { Ubiquitty::Protocol::Sync::Scope::Observe });
    Screen::setTokenID(d, cIndex, "c");
    UbiquittyTest::ScreenClient a(24, 80);
    UbiquittyTest::ScreenClient b(30, 100);
    UbiquittyTest::ScreenClient c(24, 80);
    UbiquittyTest::ScreenClient e(10, 40);

    // Runs the loop's steps until the program reads a size, or 300 ms pass.
    const auto settle = [&](WindowSize wanted) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < end) {
            Screen::endReadPass(d);
            Screen::deliver(d, aIndex, a);
            Screen::deliver(d, bIndex, b);
            Screen::deliver(d, cIndex, c);
            Screen::deliver(d, eIndex, e);
            if (programSize(fileDescriptor) == wanted)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    const auto type = [&](std::size_t index, UbiquittyTest::ScreenClient& client) {
        const std::array<std::uint8_t, 1> key { ' ' };
        CHECK(client.sendRaw(key).has_value());
        Screen::deliver(d, index, client);
    };
    const WindowSize small { .rows = 24, .columns = 80 };
    const WindowSize large { .rows = 30, .columns = 100 };
    const WindowSize pinned { .rows = 40, .columns = 120 };

    // 1. Nobody has typed: the newest window applies.
    Screen::greet(d, aIndex, a);
    Screen::greet(d, bIndex, b);
    Screen::grantAgentSession(d, *session, cIndex);
    Screen::greet(d, cIndex, c);
    CHECK(Screen::connection(d, cIndex)->state() == Sync::ConnectionState::Attached);
    CHECK_MSG(settle(large), ("before anyone types the newest window applies; the program reads " + describe(programSize(fileDescriptor))).c_str());

    // 2. Typing owns the size; pointer motion does not.
    type(aIndex, a);
    CHECK_MSG(settle(small), ("the typist's window does not apply; the program reads " + describe(programSize(fileDescriptor))).c_str());
    const std::array<Sync::PointerEvent, 1> motion { Sync::PointerEvent { .kind = Sync::PointerEventKind::Motion, .button = 0, .modifiers = 0, .row = 2, .column = 2 } };
    CHECK(b.sendPointer(motion).has_value());
    Screen::deliver(d, bIndex, b);
    CHECK_MSG(!settle(large), "pointer motion took the size");
    type(bIndex, b);
    CHECK_MSG(settle(large), ("the second typist did not take the size; the program reads " + describe(programSize(fileDescriptor))).c_str());

    // 5. Automation declaring itself a person never takes the size.
    Screen::grantAgentSession(d, *session, eIndex);
    Screen::greet(d, eIndex, e);
    CHECK(Screen::connection(d, eIndex)->state() == Sync::ConnectionState::Attached);
    type(eIndex, e);
    CHECK_MSG(!settle({ .rows = 10, .columns = 40 }), "a client the daemon verifies as automation took the size by declaring itself a person");
    CHECK_MSG(programSize(fileDescriptor) == large, ("automation moved the size; the program reads " + describe(programSize(fileDescriptor))).c_str());

    // 3. A pin holds whoever types; a caller that may not drive cannot pin.
    CHECK(c.call(R"({"jsonrpc":"2.0","id":1,"method":"pane.pinGeometry","params":{"pane":1,"rows":10,"columns":40}})"));
    Screen::deliver(d, cIndex, c);
    const std::vector<std::string> refused = c.takeResults();
    CHECK_MSG(refused.size() == 1 && refused[0].find("-32002") != std::string::npos, "a caller without the Drive scope was not answered forbidden");
    CHECK_MSG(!settle({ .rows = 10, .columns = 40 }), "a caller without the Drive scope pinned the size");
    CHECK(a.call(R"({"jsonrpc":"2.0","id":2,"method":"pane.pinGeometry","params":{"pane":1,"rows":40,"columns":120}})"));
    Screen::deliver(d, aIndex, a);
    CHECK_MSG(settle(pinned), ("the pin did not apply; the program reads " + describe(programSize(fileDescriptor))).c_str());
    // The pin names the attachment that asked, which is what tells every
    // other client whose size it is looking at.
    const Sync::GeometryStatus pinnedStatus = session->screenSource->geometry().status();
    CHECK_MSG(pinnedStatus.isPinned && Screen::connection(d, aIndex)->attachment() && pinnedStatus.pinnedBy == std::optional { Screen::connection(d, aIndex)->attachment()->attachmentID },
        "the pin does not name the attachment that asked for it");
    type(aIndex, a);
    CHECK_MSG(!settle(small), "typing moved a pinned size");
    CHECK(a.call(R"({"jsonrpc":"2.0","id":3,"method":"pane.pinGeometry","params":{"pane":1}})"));
    Screen::deliver(d, aIndex, a);
    CHECK_MSG(settle(small), ("unpinned, the owner's window does not apply; the program reads " + describe(programSize(fileDescriptor))).c_str());

    // 4. Two typists in turn every 10 ms for a second.
    const std::uint64_t resizesBefore = session->geometryChangesApplied;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (int turn = 0; std::chrono::steady_clock::now() < end; ++turn) {
        if (turn % 2 == 0)
            type(aIndex, a);
        else
            type(bIndex, b);
        Screen::endReadPass(d);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint64_t resizes = session->geometryChangesApplied - resizesBefore;
    const std::uint64_t window = Sync::measuredGeometryPolicy().coalesceMicroseconds;
    CHECK_MSG(resizes >= 2, "two typists in turn never moved the size");
    CHECK_MSG(resizes <= 1'000'000 / window + 1, (std::to_string(resizes) + " resizes in a second of two typists").c_str());

    // 6. An agent's override applies once, until a driver types.
    const WindowSize overridden { .rows = 12, .columns = 50 };
    type(aIndex, a);
    CHECK_MSG(settle(small), ("premise: the typist's window applies; the program reads " + describe(programSize(fileDescriptor))).c_str());
    CHECK(c.call(R"({"jsonrpc":"2.0","id":4,"method":"pane.overrideGeometry","params":{"pane":1,"rows":12,"columns":50}})"));
    Screen::deliver(d, cIndex, c);
    const std::vector<std::string> forbidden = c.takeResults();
    CHECK_MSG(forbidden.size() == 1 && forbidden[0].find("-32002") != std::string::npos, "an override from a caller without the Drive scope was not answered forbidden");
    CHECK_MSG(!settle(overridden), "a caller without the Drive scope set the size");
    CHECK(e.call(R"({"jsonrpc":"2.0","id":5,"method":"pane.overrideGeometry","params":{"pane":1,"rows":0,"columns":50}})"));
    Screen::deliver(d, eIndex, e);
    const std::vector<std::string> zero = e.takeResults();
    CHECK_MSG(zero.size() == 1 && zero[0].find("\"error\"") != std::string::npos, "an override to no rows was not refused");
    CHECK_MSG(!settle({ .rows = 24, .columns = 50 }), "an override to no rows moved the size");
    CHECK(e.call(R"({"jsonrpc":"2.0","id":6,"method":"pane.overrideGeometry","params":{"pane":1,"rows":12,"columns":50}})"));
    Screen::deliver(d, eIndex, e);
    const std::vector<std::string> answered = e.takeResults();
    CHECK_MSG(answered.size() == 1 && answered[0].find("\"rows\":12") != std::string::npos && answered[0].find("\"columns\":50") != std::string::npos,
        "the override was not answered with the size it set");
    CHECK_MSG(settle(overridden), ("the agent's override did not apply; the program reads " + describe(programSize(fileDescriptor))).c_str());
    type(eIndex, e);
    CHECK_MSG(!settle(small), "the agent's own typing ended its override");
    type(aIndex, a);
    CHECK_MSG(settle(small), ("a driver's typing did not take the size back; the program reads " + describe(programSize(fileDescriptor))).c_str());

    // 7. A HELLO's grid sizes a session for a person attaching alone, never
    // for an agent.
    const auto sizeAfterLoneAttach = [&](const char* tokenID, Attaching who) {
        auto* lone = Screen::spawnSession(d, 80, 24, command);
        std::array<int, 2> ends { -1, -1 };
        if (!lone || ::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) != 0)
            return WindowSize {};
        if (who == Attaching::PersonToPinnedSession && lone->screenSource) {
            lone->screenSource->geometry().pin({ .rows = 40, .columns = 120 });
            Screen::endReadPass(d);
        }
        if (who == Attaching::PersonToOverriddenSession && lone->screenSource) {
            lone->screenSource->geometry().overrideSize({ .low = 77, .high = 77 }, { .rows = 40, .columns = 120 }, Screen::now());
            Screen::endReadPass(d);
        }
        const std::size_t index = Screen::connect(d, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
        Screen::grantScope(d, index, driving);
        Screen::setTokenID(d, index, tokenID);
        if (who == Attaching::Agent)
            Screen::grantAgentSession(d, *lone, index);
        else
            Screen::markInteractiveUser(d, index);
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { index + 1, 0x67656f6d65747279 };
        greeting.requestedRole = Sync::Role::Owner;
        greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = lone->id, .expectedDaemonInstanceID = Screen::daemonInstanceID(d), .isAttachOnly = true };
        greeting.facts.viewportColumns = 50;
        greeting.facts.viewportRows = 12;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        CHECK(Sync::decodeGreet(body));
        std::vector<std::uint8_t> frame;
        CHECK(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
        Screen::receive(d, index, frame);
        CHECK(Screen::connection(d, index)->state() == Sync::ConnectionState::Attached);
        Screen::endReadPass(d);
        // The client's own end stays open: the daemon writes to it for as
        // long as the client is attached.
        return programSize(lone->stream->fileDescriptor());
    };
    const WindowSize personAttached = sizeAfterLoneAttach("person", Attaching::Person);
    CHECK_MSG(personAttached == overridden, ("premise: a person attaching alone did not take the session to its grid; the program reads " + describe(personAttached)).c_str());
    const WindowSize agentAttached = sizeAfterLoneAttach("agent", Attaching::Agent);
    CHECK_MSG(agentAttached == small, ("an agent attaching alone sized the session; the program reads " + describe(agentAttached)).c_str());
    const WindowSize pinnedAttached = sizeAfterLoneAttach("pinned", Attaching::PersonToPinnedSession);
    CHECK_MSG(pinnedAttached == pinned, ("a person attaching alone undid a pin; the program reads " + describe(pinnedAttached)).c_str());
    // The HELLO leaves an override be; the person's window takes it back
    // through the arbiter, once its `Facts` name it.
    const WindowSize overriddenAttached = sizeAfterLoneAttach("overridden", Attaching::PersonToOverriddenSession);
    CHECK_MSG(overriddenAttached == pinned, ("a person's HELLO undid an override past the arbiter; the program reads " + describe(overriddenAttached)).c_str());

    if (failures == 0)
        std::printf("geometry_follows_hands: the newest window applies until someone types, the typist owns the size and pointer motion does not take it, "
                    "a driver's pin holds and a caller without Drive is forbidden, two typists in turn resize the program at most once per window, "
                    "an agent's override holds until a driver types, and an agent attaching alone sizes nothing\n");
    return failures == 0 ? 0 : 1;
}
