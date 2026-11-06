// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/screen_lost_silence.cpp — a silent path is lost, a quiet one is
// not, and losing one client costs the others nothing.
//
// The host declares a connection Lost when bytes it sent stay outstanding
// with nothing heard for 30 s (Docs/sync.md §6.4), drops the client
// and keeps its sessions. So:
//
//   1. a client sent an update it never acknowledges is kept at 29.9 s and
//      dropped at 30 s, and its session stays;
//   2. a client in the background with nothing outstanding survives ten idle
//      minutes;
//   3. a client listed after the dropped one is still serviced in the pass
//      that dropped it.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

int main()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t silentIndex = Screen::attach(d, session.id);

    Screen::markTrustedLocal(d, silentIndex);
    const std::size_t quietIndex = Screen::attach(d, session.id);
    Screen::markTrustedLocal(d, quietIndex);
    const std::size_t laterIndex = Screen::attach(d, session.id);
    Screen::markTrustedLocal(d, laterIndex);
    UbiquittyTest::ScreenClient silent(24, 80);
    UbiquittyTest::ScreenClient quiet(24, 80);
    UbiquittyTest::ScreenClient later(24, 80);
    quiet.setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    Screen::greet(d, silentIndex, silent);
    Screen::greet(d, quietIndex, quiet);
    Screen::greet(d, laterIndex, later);
    CHECK_MSG(silent.pane() && quiet.pane() && later.pane(), "a client was not sent its checkpoint");

    // An update the silent client is sent and never acknowledges; the other
    // two read it and acknowledge.
    Ubiquitty::processBytes(session.term.get(), "output");
    session.isFrameDue = true;
    Screen::publish(d, session);
    const std::uint64_t sentAt = Screen::now();
    Screen::service(d, sentAt);
    (void)Screen::takeOutput(d, silentIndex);
    Screen::deliver(d, quietIndex, quiet);
    Screen::deliver(d, laterIndex, later);

    Screen::service(d, sentAt + 29'900'000);
    CHECK_MSG(!Screen::didOverflow(d, silentIndex), "the silent client was dropped before 30 s");

    // The pass that drops it services the client after it too.
    Ubiquitty::processBytes(session.term.get(), " more");
    session.isFrameDue = true;
    Screen::publish(d, session);
    const std::uint64_t laterUpdatesBefore = Screen::connection(d, laterIndex)->counters().chainUpdatesSent;
    Screen::service(d, sentAt + 30'100'000);
    CHECK_MSG(Screen::didOverflow(d, silentIndex), "a client silent for 30 s with bytes outstanding was kept");
    CHECK_MSG(Screen::connection(d, silentIndex)->counters().lostDeclared == 1, "the silence rule did not declare the connection Lost");
    CHECK_MSG(Screen::hasSession(d, session.id), "losing a client ended its session");
    CHECK_MSG(Screen::connection(d, laterIndex)->counters().chainUpdatesSent == laterUpdatesBefore + 1,
        "the client after the dropped one was not serviced in the same pass");

    // Ten idle minutes for the unfocused client with nothing outstanding. It
    // acknowledges what it read on its ack timer, which a quiet round of the
    // carry does not wait for: the premise is that the timer has fired.
    Screen::deliver(d, quietIndex, quiet);
    for (int rounds = 0; rounds < 200 && Screen::connection(d, quietIndex)->pacer().outstandingBytes() > 0; ++rounds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Screen::deliver(d, quietIndex, quiet);
    }
    CHECK_MSG(Screen::connection(d, quietIndex)->pacer().outstandingBytes() == 0, "premise: the idle client never acknowledged what it read");
    Screen::service(d, sentAt + 600'000'000);
    CHECK_MSG(!Screen::didOverflow(d, quietIndex), "an idle client in the background was dropped");
    CHECK_MSG(Screen::connection(d, quietIndex)->counters().probesSent == 0, "a client in the background was probed");

    if (failures == 0)
        std::printf("screen_lost_silence: a client silent with bytes outstanding is kept at 29.9 s and dropped at 30 s with its session kept, "
                    "the client after it is serviced in the same pass, and an idle client in the background survives ten minutes unprobed\n");
    return failures == 0 ? 0 : 1;
}
