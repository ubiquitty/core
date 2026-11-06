// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/overflowed_client_stays_unsynced.cpp — a client that never got a
// frame is never treated as holding it.
//
// What a client holds is what it acknowledged, and an Ack is the only thing
// that says so. Two ways a client's frames go astray here:
//
//   1. Its output hit the high-water mark: it is flagged for the drop and its
//      pending output reclaimed. The daemon queues it nothing more, screen
//      frames included.
//   2. Bytes it was queued are lost before they reach it. The host's next
//      update for it names a base it does not hold; it refuses the update,
//      asks for a checkpoint, and holds the host's screen again — a frame it
//      missed is caught on the next one, never applied onto the wrong grid.
//
// Meanwhile a client beside it, whose bytes arrive, holds the host's screen
// on every frame.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h). Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

namespace Ubiquitty::Host {

// The exact post-queueToClient overflow state: flagged, pending reclaimed.
struct OverflowedClientStaysUnsyncedAccess {
    static void stageOverflow(Daemon& d, std::size_t index)
    {
        d.m_clients[index].didOverflow = true;
        d.m_clients[index].clearPendingOutput();
    }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using A = Ubiquitty::Host::OverflowedClientStaysUnsyncedAccess;

namespace {

void writePlainRow(Ubiquitty::Terminal& t, int row, std::string_view text)
{
    Ubiquitty::processBytes(&t, "\x1b[" + std::to_string(row + 1) + ";1H");
    Ubiquitty::processBytes(&t, text);
}

} // namespace

int main()
{
    constexpr int rows = 4, columns = 20;
    Daemon d;
    auto& s = Screen::makeSession(d, rows, columns);
    const auto publish = [](Daemon& daemon, auto& session) {
        session.isFrameDue = true;
        Screen::publish(daemon, session);
    };
    const std::size_t healthy = Screen::attach(d, s.id);
    Screen::markTrustedLocal(d, healthy);
    const std::size_t overflowed = Screen::attach(d, s.id);
    Screen::markTrustedLocal(d, overflowed);
    const std::size_t lossy = Screen::attach(d, s.id);
    Screen::markTrustedLocal(d, lossy);
    UbiquittyTest::ScreenClient healthyClient(rows, columns);
    UbiquittyTest::ScreenClient overflowedClient(rows, columns);
    UbiquittyTest::ScreenClient lossyClient(rows, columns);
    Screen::greet(d, healthy, healthyClient);
    Screen::greet(d, overflowed, overflowedClient);
    Screen::greet(d, lossy, lossyClient);
    publish(d, s);
    for (const auto& [index, client] : { std::pair { healthy, &healthyClient }, std::pair { overflowed, &overflowedClient }, std::pair { lossy, &lossyClient } })
        Screen::deliver(d, index, *client);
    CHECK(healthyClient.generation() == s.screenSource->generation());
    CHECK(overflowedClient.generation() == s.screenSource->generation());
    CHECK(lossyClient.generation() == s.screenSource->generation());

    // 1. Flagged on the high-water mark, the client is queued nothing more.
    A::stageOverflow(d, overflowed);
    writePlainRow(*s.term, 0, "XXXX");
    publish(d, s);
    Screen::service(d);
    CHECK_MSG(Screen::pending(d, overflowed) == 0, "a client flagged for the drop was queued a screen frame");

    // 2. The next update reaches the lossy client's output and is lost there.
    (void)Screen::takeOutput(d, lossy);
    writePlainRow(*s.term, 1, "YYYY");
    publish(d, s);
    Screen::deliver(d, healthy, healthyClient);
    Screen::deliver(d, lossy, lossyClient);
    CHECK_MSG(lossyClient.connection().counters().resyncsSent == 1, "a client handed an update onto a frame it never got applied it or ignored it silently");
    CHECK_MSG(lossyClient.generation() == s.screenSource->generation(), "the client that lost a frame did not reach the newest one");
    const std::string lossyDifference = UbiquittyTest::firstDisagreement(*s.term, lossyClient.pane());
    CHECK_MSG(lossyDifference.empty(), lossyDifference.c_str());

    // The lossy client's checkpoint is followed by its echo context, for
    // which the host took the frame again under the next generation: every
    // client is brought to it.
    for (int rounds = 0; rounds < 100 && healthyClient.generation() != s.screenSource->generation(); ++rounds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Screen::deliver(d, healthy, healthyClient);
    }
    CHECK(healthyClient.generation() == s.screenSource->generation());
    const std::string healthyDifference = UbiquittyTest::firstDisagreement(*s.term, healthyClient.pane());
    CHECK_MSG(healthyDifference.empty(), healthyDifference.c_str());
    CHECK_MSG(healthyClient.connection().counters().resyncsSent == 0, "the healthy client asked for a resync");

    if (failures == 0)
        std::printf("overflowed_client_stays_unsynced: a client flagged for the drop is queued nothing more, one whose frame was lost refuses the next update and is checkpointed back onto the host's screen, and the client beside them never falls out of step\n");
    return failures == 0 ? 0 : 1;
}
