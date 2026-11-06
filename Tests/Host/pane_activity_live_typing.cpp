// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/pane_activity_live_typing.cpp — typing through a Sync client
// moves the pane's last-input stamp in a daemon running its own loop.
//
// pane_activity_stamps drives each stamping function through the friend seam.
// What it cannot show is that the loop calls the one that stamps Sync
// typing: that call sits at the end of a pass of Daemon::run, and nothing but
// the loop reaches it. Here the daemon runs its loop on its own thread, the
// test is a screen client on the other end of a socket pair, and the only
// input the pane ever receives is Sync raw bytes.
//
// The stamp is read on the daemon's thread, by the injected clock: each time
// the daemon asks for the time, the clock records the input stamp the session
// holds. The typed line's echo is output, so the pass that drains it asks for
// the time after the typing was stamped.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/PaneActivity.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

const Ubiquitty::Session::PaneActivity* watchedActivity = nullptr;
std::atomic<std::uint64_t> clockReads { 0 };
std::atomic<std::uint64_t> observedInputStamp { 0 };
std::atomic<std::uint64_t> observedOutputStamp { 0 };

// Runs on the daemon's thread. Every read is a later instant than the last,
// and records what the session held when it was asked.
std::uint64_t observingWallClock()
{
    observedInputStamp.store(watchedActivity->lastInputUnixMilliseconds);
    observedOutputStamp.store(watchedActivity->lastOutputUnixMilliseconds);
    return 1'000 * (clockReads.fetch_add(1) + 1);
}

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

} // namespace

int main()
{
    Daemon d;
    // Reads two lines and exits; the terminal echoes each as it is typed.
    const std::array<std::string, 3> command { "/bin/sh", "-c", "read first; read second" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    watchedActivity = &session->activity;

    std::array<int, 2> ends {};
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0, "premise: no socket pair");
    (void)::fcntl(ends[0], F_SETFL, ::fcntl(ends[0], F_GETFL) | O_NONBLOCK);
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    const std::size_t index = Screen::attach(d, session->id, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, "typist");
    Screen::markInteractiveUser(d, index);
    Screen::setWallClock(d, &observingWallClock);

    // The client greets for the session the attach selected; its Greet is
    // the first thing it writes. A connection that never greets is reaped at
    // the end of the hello grace, and the session it leaves keeps the loop
    // running past the join.
    UbiquittyTest::ScreenClient client(24, 80);
    const auto request = Screen::selectedRequest(d, index);
    const bool hasStarted = request && client.start(*request);
    CHECK_MSG(hasStarted, "premise: the client started with the attach's request");
    if (!hasStarted)
        return UBIQUITTY_TEST_SUMMARY();

    // A daemon that was never started names no socket, and its Welcome would
    // carry a selection without one; the loop serves under fixture metadata.
    const Screen::FixtureServingMetadata serving(d);
    // From here the daemon is its own: the test touches it through the socket
    // and the clock only.
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

    CHECK_MSG(exchangeUntil([&] { return client.pane() != nullptr; }), "premise: the client was bound to the pane");

    // The first line, as raw bytes over Sync and nothing else.
    const std::array<std::uint8_t, 2> line { 'x', '\n' };
    bool wasTyped = false;
    const bool wasStamped = exchangeUntil([&] {
        if (!wasTyped)
            wasTyped = client.sendRaw(line).has_value();
        return observedInputStamp.load() != 0;
    });
    CHECK_MSG(wasTyped, "premise: the client could type");
    CHECK_MSG(wasStamped, "the running loop stamped the Sync typing");
    const std::uint64_t stamp = observedInputStamp.load();
    CHECK_MSG(stamp % 1'000 == 0 && stamp <= 1'000 * clockReads.load(), "the stamp is an instant the injected clock gave");

    // The second line ends the program, and the loop with its last session.
    bool wasEnded = false;
    (void)exchangeUntil([&] {
        if (!wasEnded)
            wasEnded = client.sendRaw(line).has_value();
        return wasEnded;
    });
    for (int round = 0; round < 50; ++round)
        exchange(descriptor, client);
    ::close(descriptor);
    loop.join();
    return UBIQUITTY_TEST_SUMMARY();
}
