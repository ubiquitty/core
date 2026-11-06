// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/resize_storm.cpp — a storm of Viewports in one read pass costs
// one reflow, one SIGWINCH and one checkpoint.
//
// A window dragged across a screen sends a Viewport per mouse event, and one
// read pass can carry thousands of them. The geometry that counts is the newest:
// the size follows the hands that sent it last (Docs/sync.md §6.10). So the
// daemon applies the pass's newest geometry once, when the pass ends, and a
// client's connection is sent one checkpoint of it. Input that follows a
// Viewport in the same pass still reaches a program that was told of it.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

int main()
{
    constexpr int stormFrames = 2'600;
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    for (int line = 0; line < 60; ++line)
        Ubiquitty::processBytes(session.term.get(), "history line " + std::to_string(line) + " long enough to wrap when the window narrows\r\n");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session.id);
    // A Viewport drives the session, so the client holds the scope that
    // allows it, in a token minted for the person at the window, as a human
    // client's is: only a person's client has a viewport of a pane.
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    CHECK_MSG(client.pane(), "the client was not sent its checkpoint");
    const std::uint64_t checkpointsBefore = Screen::connection(d, index)->counters().checkpointsSent;

    // A drag: every Viewport a different geometry from the one before, the
    // last one 100x30. All of them arrive in one read pass.
    int sent = 0;
    for (int frame = 0; frame < stormFrames - 1; ++frame)
        sent += client.sendViewport(60 + frame % 40, 20 + frame % 9) ? 1 : 0;
    sent += client.sendViewport(100, 30) ? 1 : 0;
    CHECK_MSG(sent == stormFrames, ("premise: the client sent " + std::to_string(sent) + " of the storm's viewports").c_str());
    const std::vector<std::uint8_t> storm = client.takeStream(Screen::now());
    CHECK_MSG(storm.size() <= 64 * 1024, ("premise: the storm is " + std::to_string(storm.size()) + " bytes, more than one read pass takes").c_str());
    Screen::receive(d, index, storm);
    Screen::endReadPass(d);

    CHECK_MSG(pty.windowSizeCalls == 1, ("the storm sent " + std::to_string(pty.windowSizeCalls) + " SIGWINCHes").c_str());
    CHECK_MSG(session.geometryChangesApplied == 1, ("the storm reflowed " + std::to_string(session.geometryChangesApplied) + " times").c_str());
    CHECK_MSG(session.term->columns == 100 && session.term->rows == 30, "the storm did not end at its last geometry");
    CHECK_MSG(pty.columns == 100 && pty.rows == 30, "the program was told another geometry than the last");

    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    Screen::deliver(d, index, client);
    const std::uint64_t checkpoints = Screen::connection(d, index)->counters().checkpointsSent - checkpointsBefore;
    CHECK_MSG(checkpoints == 1, ("the client was sent " + std::to_string(checkpoints) + " checkpoints for the storm").c_str());
    CHECK_MSG(client.pane() && client.pane()->columns == 100 && client.pane()->rows == 30, "the client does not hold the last geometry");
    const std::string difference = client.pane() ? UbiquittyTest::firstDisagreement(*session.term, *client.pane()) : std::string("no pane");
    CHECK_MSG(difference.empty(), difference.c_str());

    // Input after a Viewport in the same pass reaches a program already told.
    const std::array<std::uint8_t, 1> typed { 'x' };
    CHECK(client.sendViewport(90, 28));
    CHECK(client.sendRaw(typed).has_value());
    const std::size_t writtenBefore = pty.written.size();
    Screen::receive(d, index, client.takeStream(Screen::now()));
    CHECK_MSG(pty.columns == 90 && pty.rows == 28, "input reached the program before the Viewport ahead of it");
    CHECK_MSG(pty.written.size() == writtenBefore + 1, "the input did not reach the program");
    CHECK_MSG(pty.windowSizeCalls == 2, ("the Viewport before input sent " + std::to_string(pty.windowSizeCalls - 1) + " SIGWINCHes").c_str());

    if (failures == 0)
        std::printf("resize_storm: %d Viewports in one read pass cost one SIGWINCH, one reflow and one checkpoint, and "
                    "input behind a Viewport reaches a program that was told of it\n",
            stormFrames);
    return failures == 0 ? 0 : 1;
}
