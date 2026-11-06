// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/two_client_pacing.cpp — a slow client never stalls a fast one.
//
// Two clients share one session. One reads and acknowledges everything at
// once; the other reads nothing. Each has its own Sync connection, so its own
// window, its own baselines and its own place in the stream:
//
//   1. the fast client holds the host's screen after every frame;
//   2. the slow client is queued less than two frames' worth of output: the
//      daemon encodes for a client only while its output holds less than one
//      capped frame, and the last frame it takes may be a whole one. It is
//      never dropped, and is owed rather than fed;
//   3. once it reads, it is brought to the newest frame in one update — a
//      jump to now, not a replay of the frames it missed.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// slow_client_withhold. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdint>
#include <cstdio>
#include <string>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

int main()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t fastIndex = Screen::attach(d, session.id);

    Screen::markTrustedLocal(d, fastIndex);
    const std::size_t slowIndex = Screen::attach(d, session.id);
    Screen::markTrustedLocal(d, slowIndex);
    UbiquittyTest::ScreenClient fast(24, 80);
    UbiquittyTest::ScreenClient slow(24, 80);
    Screen::greet(d, fastIndex, fast);
    Screen::greet(d, slowIndex, slow);
    CHECK_MSG(fast.pane() && slow.pane(), "a client was not sent its checkpoint");
    const std::uint64_t slowSyncedAt = slow.generation();

    // The slow client stops reading here. Every frame is a scrolled screen of
    // fresh text, so each update carries rows and history.
    std::size_t largestSlowOutput = 0;
    for (int frame = 0; frame < 400; ++frame) {
        std::string text = "frame " + std::to_string(frame) + ": ";
        text.append(60, static_cast<char>('a' + frame % 26));
        text += "\r\n";
        Ubiquitty::processBytes(session.term.get(), text);
        session.isFrameDue = true;
        Screen::publish(d, session);
        Screen::deliver(d, fastIndex, fast);

        CHECK_MSG(fast.generation() == session.screenSource->generation(), ("the fast client fell behind at frame " + std::to_string(frame)).c_str());
        const std::string difference = UbiquittyTest::firstDisagreement(*session.term, fast.pane());
        CHECK_MSG(difference.empty(), ("frame " + std::to_string(frame) + ": " + difference).c_str());
        largestSlowOutput = std::max(largestSlowOutput, Screen::pending(d, slowIndex));
        CHECK_MSG(!Screen::didOverflow(d, slowIndex), "the slow client was dropped");
    }
    CHECK_MSG(largestSlowOutput > 0, "premise: the slow client was queued nothing at all");
    CHECK_MSG(largestSlowOutput < 2 * Screen::lowWaterBytes(),
        ("the slow client's output reached " + std::to_string(largestSlowOutput) + " bytes; two frames are " + std::to_string(2 * Screen::lowWaterBytes())).c_str());
    CHECK_MSG(slow.generation() == slowSyncedAt, "premise: the slow client read something while it was not reading");

    // It reads again: what it had queued, then one update to the newest frame.
    const std::uint64_t sentWhileStalled = Screen::connection(d, slowIndex)->counters().chainUpdatesSent;
    // How many updates fill the slow client's output is a matter of their
    // size: a scroll travels as one ScrollRect and the row it reveals, so a
    // few hundred fit where a few dozen whole screens did. What is held is
    // the bound above; what matters here is that frames were skipped.
    CHECK_MSG(sentWhileStalled < 400,
        ("the slow client was sent " + std::to_string(sentWhileStalled) + " updates for 400 frames; a paced client is owed, not fed").c_str());
    Screen::deliver(d, slowIndex, slow);
    CHECK_MSG(slow.generation() == session.screenSource->generation(), "the slow client did not reach the newest frame once it read");
    const std::string difference = UbiquittyTest::firstDisagreement(*session.term, slow.pane());
    CHECK_MSG(difference.empty(), difference.c_str());
    const std::uint64_t sentToCatchUp = Screen::connection(d, slowIndex)->counters().chainUpdatesSent - sentWhileStalled;
    CHECK_MSG(sentToCatchUp == 1, ("the slow client was sent " + std::to_string(sentToCatchUp) + " updates to catch up; one reaches the newest frame").c_str());
    CHECK_MSG(Screen::connection(d, slowIndex)->counters().checkpointsSent == 1, "the slow client was sent a checkpoint to catch up");
    CHECK_MSG(slow.connection().counters().resyncsSent == 0, "the slow client asked for a resync");

    if (failures == 0)
        std::printf("two_client_pacing: a client that stops reading holds less than two frames of output, is sent %llu updates "
                    "over 400 frames and is never dropped; the client beside it holds the host's screen on every frame, and "
                    "the slow one reaches the newest frame in one update once it reads\n",
            static_cast<unsigned long long>(sentWhileStalled));
    return failures == 0 ? 0 : 1;
}
