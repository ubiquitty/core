// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/ack_rides_input.cpp — an Ack the client owes leaves in the same
// carrier write as the input typed after it.
//
// A pending Ack leaves in the same carrier write as outgoing input
// (Docs/sync.md §6.4), so a keystroke costs the uplink one write, not an Ack
// and then the input. Driven end to end: the client stack the native apps use,
// a RemoteSessionCore over `ubiquitty-host --attach`, to a real forked daemon,
// with a carrier that keeps each write the client hands it.
//
// The shell is asked to print two seconds after the command's echo. The client's
// clock is stopped and its timers held before that output arrives, so the Ack
// it owes for it cannot fall due; the client types; and:
//
//   1. no write between the output's arrival and the keystroke carries an Ack;
//   2. the keystroke goes in exactly one write, which carries the Ack.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/RemoteLinkHarness.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using namespace std::chrono_literals;
namespace Harness = Ubiquitty::HostTest;

namespace {

/// Counts the pictures the core applies.
class ScreenCount final : public Session::RemoteSessionObserver {
public:
    void onScreenChanged() override { ++screens; }
    int screens = 0;
};

/// The kinds of the frames in one write, in order.
///
/// - Parameter write: Whole canonical frames.
/// - Returns: Their kinds.
std::vector<Protocol::Sync::FrameKind> kindsOf(std::span<const std::uint8_t> write)
{
    std::vector<Protocol::Sync::FrameKind> kinds;
    Protocol::Sync::FrameDecoder decoder;
    decoder.append(write);
    while (const std::optional<Protocol::Sync::Frame> frame = decoder.next())
        kinds.push_back(frame->kind);
    return kinds;
}

/// Whether a write carries a frame of a kind.
///
/// - Parameters:
///   - write: Whole canonical frames.
///   - kind: The kind.
/// - Returns: Whether one is among them.
bool carries(std::span<const std::uint8_t> write, Protocol::Sync::FrameKind kind)
{
    const std::vector<Protocol::Sync::FrameKind> kinds = kindsOf(write);
    return std::ranges::find(kinds, kind) != kinds.end();
}

/// Drives one session through the daemon and holds its owed Ack to the
/// keystroke after it.
///
/// - Parameter hostBinary: The built `ubiquitty-host`.
void anOwedAckRidesTheKeystroke(const std::string& hostBinary)
{
    Harness::ForkedDaemon daemon(hostBinary);
    REQUIRE_MSG(daemon.isReady(), "premise: the daemon did not start");
    Harness::PumpedDispatcher dispatcher;
    Harness::StoppableClock clock;
    const auto log = std::make_shared<Harness::WriteLog>();
    Harness::AttachChannelFactory factory(daemon.attachCommand(), daemon.socketName(), log);
    ScreenCount observer;
    auto terminal = std::make_unique<Terminal>();
    Ubiquitty::resize(*terminal, 24, 80);
    Session::RemoteSessionCore core(terminal.get(), &dispatcher, &factory, &observer);
    core.setClock([&clock] { return clock.now(); });
    core.start(0, 80, 24);
    const auto shows = [&](std::string_view text) { return Harness::gridText(*terminal).find(text) != std::string::npos; };
    REQUIRE_MSG(Harness::pumpUntil(dispatcher, 10s, [&] { return core.acknowledged() && observer.screens > 0; }), "premise: the client did not attach");

    // The command's echo spells the word apart; only its output spells it whole.
    core.sendInput("sleep 2; echo LA''TE\r");
    REQUIRE_MSG(Harness::pumpUntil(dispatcher, 5s, [&] { return shows("LA''TE"); }), "premise: the shell did not echo the command");
    // The echo's acknowledgements leave on their own timers.
    (void)Harness::pumpUntil(dispatcher, 300ms, [] { return false; });

    clock.freeze();
    dispatcher.setTimers(Harness::Timers::Held);
    REQUIRE_MSG(!shows("LATE"), "premise: the shell's output arrived before the client's clock stopped");
    const std::size_t writesAtFreeze = log->size();
    REQUIRE_MSG(Harness::pumpUntil(dispatcher, 10s, [&] { return shows("LATE"); }), "premise: the shell's output did not arrive");
    for (std::size_t index = writesAtFreeze; index < log->size(); ++index)
        CHECK_MSG(!carries(log->at(index), Protocol::Sync::FrameKind::Ack), "an Ack left in a write of its own before the keystroke");

    // A comment, so the line the shell holds runs nothing.
    const std::size_t writesBeforeInput = log->size();
    core.sendInput("#ackrides");
    const std::size_t inputWrites = log->size() - writesBeforeInput;
    CHECK_MSG(inputWrites == 1, ("the keystroke took " + std::to_string(inputWrites) + " writes").c_str());
    if (inputWrites >= 1) {
        const std::vector<std::uint8_t> write = log->at(writesBeforeInput);
        CHECK_MSG(carries(write, Protocol::Sync::FrameKind::Raw), "the keystroke's write carries no input");
        CHECK_MSG(carries(write, Protocol::Sync::FrameKind::Ack), "the Ack owed for the shell's output did not leave with the keystroke");
    }

    clock.thaw();
    dispatcher.setTimers(Harness::Timers::Running);
    CHECK_MSG(Harness::pumpUntil(dispatcher, 5s, [&] { return shows("#ackrides"); }), "the keystroke did not reach the shell");
    core.stop();
    dispatcher.pump();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    // A carrier whose bridge has gone is an error to read, not a signal.
    (void)::signal(SIGPIPE, SIG_IGN);
    anOwedAckRidesTheKeystroke(argv[1]);
    if (failures == 0)
        std::printf("ack_rides_input: the Ack owed for output that arrived while the client's timers were held left in the "
                    "one write that carried the keystroke after it, and in no write before\n");
    return failures == 0 ? 0 : 1;
}
