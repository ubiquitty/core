// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/echo_one_write.cpp — a keystroke's echo leaves in one write.
//
// Every frame for a connection from one publish pass leaves in one gathered
// write (Docs/sync.md §6.5): a write per frame would let the second wait a
// delayed-ACK interval behind the first, so a keystroke's echo costs a packet
// per frame and a round of latency. Two cases:
//
//   1. The echo is a frame of two: the update that shows the typed character
//      and the title the same output set. The loop queues both before its
//      flush, so the client's socket takes one write carrying both.
//   2. Two sessions publish in one pass to two clients, each attached to the
//      workspace that holds both: each client's socket takes one write
//      carrying both sessions' updates and titles.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// Reads everything waiting on a non-blocking socket.
///
/// - Parameter fileDescriptor: The socket.
/// - Returns: The bytes.
std::vector<std::uint8_t> drain(int fileDescriptor)
{
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 65536> buffer {};
    for (;;) {
        const ssize_t n = ::read(fileDescriptor, buffer.data(), buffer.size());
        if (n <= 0)
            return bytes;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + n);
    }
}

/// Carries the screen plane both ways through the client's socket, as the
/// loop and a transport do, until neither end has more to say.
///
/// - Parameters:
///   - d: The daemon.
///   - index: The client.
///   - peer: The client's end of the socket.
///   - client: The client.
void deliver(Daemon& d, std::size_t index, int peer, UbiquittyTest::ScreenClient& client)
{
    for (int round = 0; round < 1024; ++round) {
        const std::uint64_t at = Screen::now();
        Screen::service(d, at);
        Screen::flush(d, index);
        const std::vector<std::uint8_t> down = drain(peer);
        client.read(down, at);
        const std::vector<std::uint8_t> up = client.takeStream(at);
        if (!up.empty())
            Screen::receive(d, index, up);
        if (down.empty() && up.empty())
            return;
    }
}

/// Case 1: a keystroke's echo, an update and its title, leaves in one write.
void aKeystrokeEchoLeavesInOneWrite()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    std::array<int, 2> sockets {};
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "premise: a socket pair");
    for (const int end : sockets)
        (void)::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
    Ubiquitty::PAL::UnixFileDescriptor peer(sockets[1]);
    const std::size_t index = Screen::attach(d, session.id, Ubiquitty::PAL::UnixFileDescriptor(sockets[0]));
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    const auto request = Screen::selectedRequest(d, index);
    CHECK_MSG(request.has_value(), "premise: the actual session and daemon were selected");
    if (!request || !client.start(*request))
        return;
    Screen::receive(d, index, client.takeStream(Screen::now()));
    deliver(d, index, peer.get(), client);
    // The pane is captured only once a client shows it, so its first frame
    // is taken now, and the client holds that checkpoint before it types.
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    deliver(d, index, peer.get(), client);
    CHECK_MSG(client.pane() && client.generation() == session.screenSource->generation(), "the client was not sent its checkpoint");

    // The keystroke reaches the program; what the daemon wrote for it is not
    // the echo.
    const std::array<std::uint8_t, 1> typed { 'x' };
    CHECK_MSG(client.sendRaw(typed).has_value(), "premise: the client's keystroke was taken");
    Screen::receive(d, index, client.takeStream(Screen::now()));
    Screen::flush(d, index);
    client.read(drain(peer.get()), Screen::now());

    // The program echoes and sets its title; the loop publishes, services the
    // connection and flushes, once.
    const std::uint64_t writesBefore = Screen::outputWrites(d, index);
    const std::size_t updatesBefore = client.count(Ubiquitty::Protocol::Sync::FrameKind::Update);
    const std::size_t factsBefore = client.count(Ubiquitty::Protocol::Sync::FrameKind::PaneFacts);
    Ubiquitty::processBytes(session.term.get(), "x\x1b]0;typing\x07");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Settled);
    Screen::service(d);
    Screen::flush(d, index);
    const std::uint64_t writes = Screen::outputWrites(d, index) - writesBefore;
    client.read(drain(peer.get()), Screen::now());
    const std::size_t updates = client.count(Ubiquitty::Protocol::Sync::FrameKind::Update) - updatesBefore;
    const std::size_t facts = client.count(Ubiquitty::Protocol::Sync::FrameKind::PaneFacts) - factsBefore;

    CHECK_MSG(updates == 1, ("premise: the echo was " + std::to_string(updates) + " updates").c_str());
    CHECK_MSG(facts >= 1, "premise: the canonical title facts did not travel with the echo");
    const auto* title = client.connection().paneFacts(1);
    CHECK_MSG(title && title->programTitle == "typing", "the typed facts did not preserve the program's title");
    CHECK_MSG(writes == 1, ("the echo's " + std::to_string(updates + facts) + " frames took " + std::to_string(writes) + " writes").c_str());
    CHECK_MSG(client.pane() && UbiquittyTest::firstDisagreement(*session.term, *client.pane()).empty(), "the client does not show the echo");

    if (failures == 0)
        std::printf("echo_one_write: a keystroke's echo, %zu frames, left in one write\n", updates + facts);
}

/// One client of the two-session case: its socket, the daemon's index for it,
/// and the handles it knows each session by.
struct SessionPairReader {
    explicit SessionPairReader(Sync::ID128 instance)
        : client(24, 80, instance)
    {
    }

    Ubiquitty::PAL::UnixFileDescriptor peer;
    std::size_t index = 0;
    std::array<std::uint32_t, 2> handles {};
    UbiquittyTest::ScreenClient client;
};

/// Case 2: two sessions publish in one pass to two clients attached to the
/// workspace that holds both; each client's socket takes one write.
void twoSessionsPublishedTogetherLeaveInOneWriteForEachClient()
{
    Daemon d;
    auto& left = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    auto& right = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    const std::array<std::remove_reference_t<decltype(left)>*, 2> sessions { &left, &right };
    for (auto* session : sessions) {
        Ubiquitty::processBytes(session->term.get(), "$ ");
        Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
    }
    (void)Screen::nameWorkspace(d, left, "pair");
    (void)Screen::tabInto(d, left, right);

    SessionPairReader first(Sync::ID128 { .low = 21, .high = 1 });
    SessionPairReader second(Sync::ID128 { .low = 22, .high = 1 });
    const std::array<SessionPairReader*, 2> readers { &first, &second };
    for (std::size_t reader = 0; reader < readers.size(); ++reader) {
        SessionPairReader& each = *readers[reader];
        std::array<int, 2> sockets {};
        REQUIRE_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "two sessions: premise: a socket pair");
        for (const int end : sockets)
            (void)::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
        each.peer = Ubiquitty::PAL::UnixFileDescriptor(sockets[1]);
        each.index = Screen::attach(d, left.id, Ubiquitty::PAL::UnixFileDescriptor(sockets[0]));
        Screen::grantScope(d, each.index, { Sync::Scope::Observe });
        Screen::setTokenID(d, each.index, reader == 0 ? "first reader" : "second reader");
        Screen::markInteractiveUser(d, each.index);
        Screen::bindWorkspace(d, each.index, left);
        const auto request = Screen::selectedRequest(d, each.index);
        REQUIRE_MSG(request && each.client.start(*request), "two sessions: premise: a client did not start");
        Screen::receive(d, each.index, each.client.takeStream(Screen::now()));
        deliver(d, each.index, each.peer.get(), each.client);
    }
    // The panes are captured once a client shows them; their first frames
    // go now.
    for (auto* session : sessions)
        Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
    // Twice round, so neither client holds frames queued while the other was
    // carried.
    for (int round = 0; round < 2; ++round) {
        for (SessionPairReader* each : readers)
            deliver(d, each->index, each->peer.get(), each->client);
    }
    for (SessionPairReader* reader : readers) {
        SessionPairReader& each = *reader;
        for (std::size_t place = 0; place < sessions.size(); ++place) {
            for (std::uint32_t handle = 1; handle <= sessions.size(); ++handle) {
                if (const auto pane = each.client.boundPane(handle); pane && pane->low == sessions[place]->id)
                    each.handles[place] = handle;
            }
            REQUIRE_MSG(each.handles[place] && each.client.connection().paneGeneration(each.handles[place]) == sessions[place]->screenSource->generation(),
                "two sessions: premise: a client does not hold both sessions");
        }
    }

    // Both programs write and retitle; the loop publishes both, services every
    // connection and flushes each, once.
    std::array<std::uint64_t, 2> writesBefore {};
    std::array<std::size_t, 2> updatesBefore {};
    for (std::size_t reader = 0; reader < readers.size(); ++reader) {
        writesBefore[reader] = Screen::outputWrites(d, readers[reader]->index);
        updatesBefore[reader] = readers[reader]->client.count(Sync::FrameKind::Update);
    }
    Ubiquitty::processBytes(left.term.get(), "left\x1b]0;left side\x07");
    Ubiquitty::processBytes(right.term.get(), "right\x1b]0;right side\x07");
    for (auto* session : sessions) {
        session->isFrameDue = true;
        Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Settled);
    }
    Screen::service(d);
    for (const SessionPairReader* each : readers)
        Screen::flush(d, each->index);

    for (std::size_t reader = 0; reader < readers.size(); ++reader) {
        SessionPairReader& each = *readers[reader];
        const std::uint64_t writes = Screen::outputWrites(d, each.index) - writesBefore[reader];
        each.client.read(drain(each.peer.get()), Screen::now());
        const std::size_t updates = each.client.count(Sync::FrameKind::Update) - updatesBefore[reader];
        const std::string who = "two sessions, reader " + std::to_string(reader) + ": ";
        CHECK_MSG(updates == 2, (who + "premise: the pass sent " + std::to_string(updates) + " updates, not one for each session").c_str());
        CHECK_MSG(writes == 1, (who + "one publish of two sessions took " + std::to_string(writes) + " writes").c_str());
        for (std::size_t place = 0; place < sessions.size(); ++place) {
            const std::string difference = UbiquittyTest::firstDisagreement(*sessions[place]->term, each.client.connection().paneTerminal(each.handles[place]));
            CHECK_MSG(difference.empty(), (who + difference).c_str());
        }
        const auto* leftTitle = each.client.connection().paneFacts(each.handles[0]);
        const auto* rightTitle = each.client.connection().paneFacts(each.handles[1]);
        CHECK_MSG(leftTitle && leftTitle->programTitle == "left side" && rightTitle && rightTitle->programTitle == "right side",
            (who + "the titles did not travel with the pass").c_str());
    }
    if (failures == 0)
        std::printf("echo_one_write: one publish of two sessions left in one write for each of two clients\n");
}

} // namespace

int main()
{
    // As the daemon's own main does: a peer gone mid-write is an error to
    // read, not a signal.
    (void)::signal(SIGPIPE, SIG_IGN);
    aKeystrokeEchoLeavesInOneWrite();
    twoSessionsPublishedTogetherLeaveInOneWriteForEachClient();
    return failures == 0 ? 0 : 1;
}
