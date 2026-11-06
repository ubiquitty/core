// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/pane_end_facts.cpp — how a session ended is in the last facts
// its readers are sent, from a daemon running its own loop.
//
// The end is read where the loop reaps a session, once the child's exit
// status is known, and nothing but the loop reaches that. Here the daemon
// runs its loop on its own thread and the test is a screen client on the
// other end of a socket pair. Two programs: one exits with a status of its
// own, one is killed by a signal.
//
// Before either ends, a pane that has written nothing since it was bound has
// its facts read for the reader that arrived: the facts reach the client
// with no output to carry them.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Records.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

// One exchange between the client and the daemon's end of the socket: what
// the daemon sent is read, and what the client has to say is written.
void exchange(int descriptor, UbiquittyTest::ScreenClient& client)
{
    struct pollfd waiting { descriptor, POLLIN, 0 };
    if (::poll(&waiting, 1, 10) > 0 && (waiting.revents & POLLIN)) {
        std::array<std::uint8_t, 65536> bytes {};
        const ssize_t count = ::read(descriptor, bytes.data(), bytes.size());
        if (count > 0)
            client.read(std::span { bytes.data(), static_cast<std::size_t>(count) }, Screen::now());
    }
    const std::vector<std::uint8_t> up = client.takeUpstream(Screen::now());
    std::size_t written = 0;
    while (written < up.size()) {
        const ssize_t count = ::write(descriptor, up.data() + written, up.size() - written);
        if (count <= 0)
            break;
        written += static_cast<std::size_t>(count);
    }
}

/// Runs a program in a daemon of its own, ends it with one typed line, and
/// returns the last facts a client bound to its pane was sent.
///
/// - Parameter script: What `/bin/sh -c` runs; it reads a line and ends.
/// - Returns: The facts, or nothing when the client was never sent any.
std::optional<Sync::PaneFacts> lastFactsOf(const std::string& script)
{
    Daemon d;
    const std::array<std::string, 3> command { "/bin/sh", "-c", script };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return std::nullopt;

    std::array<int, 2> ends {};
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0, "premise: no socket pair");
    (void)::fcntl(ends[0], F_SETFL, ::fcntl(ends[0], F_GETFL) | O_NONBLOCK);
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    const std::size_t index = Screen::attach(d, session->id, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, "reader");
    Screen::markInteractiveUser(d, index);
    // The client greets the target its attach selected, read before the loop
    // owns the daemon.
    UbiquittyTest::ScreenClient client(24, 80);
    const auto request = Screen::selectedRequest(d, index);
    CHECK_MSG(request && client.start(*request), "premise: the client could not greet its session");
    if (!request || !client.hasStarted())
        return std::nullopt;

    // A daemon that was never started names no socket, and its Welcome would
    // carry a selection without one; the loop serves under fixture metadata.
    const Screen::FixtureServingMetadata serving(d);
    std::thread loop([&d] { (void)d.run(); });

    const int descriptor = ends[1];
    const auto exchangeUntil = [&](auto&& isDone) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            exchange(descriptor, client);
            if (isDone())
                return true;
        }
        return false;
    };

    // The program waits on a read and writes nothing: the facts that arrive
    // were read for the reader, not for a frame.
    CHECK_MSG(exchangeUntil([&] { return client.connection().paneFacts(1) != nullptr; }), "a pane bound by a reader was sent no facts until it wrote");
    if (const Sync::PaneFacts* facts = client.connection().paneFacts(1))
        CHECK_MSG(!facts->end.didEnd && facts->rows == 24 && facts->columns == 80, "the facts of a running pane say it ended, or carry no grid");

    // The pane is unbound once its session is gone, so the facts are kept as
    // they arrive.
    std::optional<Sync::PaneFacts> facts;
    client.observeAppliedFrames([&](const Sync::ClientConnection& connection) {
        if (const Sync::PaneFacts* held = connection.paneFacts(1))
            facts = *held;
    });
    const std::array<std::uint8_t, 2> line { 'x', '\n' };
    bool wasTyped = false;
    (void)exchangeUntil([&] {
        if (!wasTyped)
            wasTyped = client.sendRaw(line).has_value();
        return wasTyped && facts && facts->end.didEnd;
    });
    CHECK_MSG(wasTyped, "premise: the client could type");
    for (int round = 0; round < 50; ++round)
        exchange(descriptor, client);
    ::close(descriptor);
    loop.join();
    client.observeAppliedFrames({});
    return facts;
}

} // namespace

int main()
{
    const std::optional<Sync::PaneFacts> exited = lastFactsOf("read line; exit 3");
    CHECK_MSG(exited.has_value(), "a reader of a pane whose program exited was sent no facts");
    if (exited)
        CHECK_MSG((exited->end == Sync::SessionEndFacts { .didEnd = true, .hasStatus = true, .wasSignalled = false, .code = 3 }), "the last facts do not say the program exited with its status");

    const std::optional<Sync::PaneFacts> killed = lastFactsOf("read line; kill -KILL $$");
    CHECK_MSG(killed.has_value(), "a reader of a pane whose program was killed was sent no facts");
    if (killed)
        CHECK_MSG((killed->end == Sync::SessionEndFacts { .didEnd = true, .hasStatus = true, .wasSignalled = true, .code = 9 }), "the last facts do not say a signal ended the program");
    return UBIQUITTY_TEST_SUMMARY();
}
