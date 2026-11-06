// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/input_settled_echo.cpp — a frame the daemon sends never reports
// input settled before it holds the echo (Docs/sync.md §6.7). The host twin
// of test_sync_input's echo case, on a real PTY.
//
// The program reads a byte, sleeps 30 ms and echoes it. A client attached
// over Sync with the Drive scope sends `Z`. The loop's steps run by hand on
// real time: the PTY is drained when readable, input due to settle is checked
// against a quiet PTY, frames are published and sent. Every time the client
// holds a report of the key settled, its replica holds the `Z`; and the key
// does settle. Friend seam, the QUIC-off host build, as echo_one_write.
//

#include "Assertions.h"

#include "Core/Runtime/RemoteInput.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

bool firstRowHolds(const Ubiquitty::Terminal& terminal, char letter)
{
    for (int column = 0; column < terminal.columns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, 0, column);
        if (cell && cell->codePoint == static_cast<std::uint32_t>(letter))
            return true;
    }
    return false;
}

} // namespace

int main()
{
    Daemon d;
    const std::array<std::string, 3> command { "/bin/sh", "-c", "stty -echo -icanon min 1; while c=$(dd bs=1 count=1 2>/dev/null); do sleep 0.03; printf %s \"$c\"; done" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    const std::size_t index = Screen::attach(d, session->id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    // Let the program start and settle into its read.
    const auto pump = [&](std::chrono::milliseconds span, const auto& afterStep) {
        const auto end = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < end) {
            pollfd descriptor { .fd = session->stream->fileDescriptor(), .events = POLLIN, .revents = 0 };
            if (::poll(&descriptor, 1, 2) > 0 && (descriptor.revents & POLLIN))
                Screen::servicePTY(d, *session);
            Screen::settleDueInput(d);
            if (session->isFrameDue || !session->screenSource->isFrameHeld())
                Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Settled);
            Screen::deliver(d, index, client);
            afterStep();
        }
    };
    pump(std::chrono::milliseconds(300), [] { });
    CHECK_MSG(client.pane() != nullptr, "premise: the client was not bound to the pane");
    if (!client.pane())
        return UBIQUITTY_TEST_SUMMARY();
    CHECK_MSG(!firstRowHolds(*client.pane(), 'Z'), "premise: the screen held a Z before it was typed");

    const std::array<std::uint8_t, 1> typed { 'Z' };
    CHECK_MSG(client.sendRaw(typed).has_value(), "premise: the client did not send the key");
    Screen::receive(d, index, client.takeStream(Screen::now()));
    int settledFrames = 0;
    pump(std::chrono::milliseconds(400), [&] {
        if (client.connection().receipt(1).inputSettled < 1)
            return;
        ++settledFrames;
        CHECK_MSG(firstRowHolds(*client.pane(), 'Z'), "the client held a report of the key settled before its echo");
    });
    CHECK_MSG(settledFrames > 0, "the key never settled");
    CHECK_MSG(client.connection().receipt(1).inputWritten == 1, "the key was not reported written");

    std::printf("input_settled_echo: the key settled only with its echo in the picture\n");
    return UBIQUITTY_TEST_SUMMARY();
}
