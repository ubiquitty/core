// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/echo_context_facts.cpp — the daemon samples what a client's
// predictor reads of a pane once per frame (Docs/sync.md §6.7 "Echo
// context"): the PTY's line discipline, the shell's prompt marks and the
// foreground program.
//
// A program turns echo off at a prompt: the client's echo context says
// secure entry, at the prompt, with the line discipline's interrupt
// characters. Echo back on, the next frame says so. Echo turned off with no
// output is found when the client's input is written, and a frame carries it.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

int main()
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    pty.facts.discipline = Ubiquitty::PAL::LineDiscipline { .isEchoEnabled = false, .isCanonical = true, .isSignalEnabled = true, .isInputDiscardedOnSignal = true, .interruptCharacter = 0x03, .quitCharacter = 0x1c, .suspendCharacter = std::nullopt };
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Ubiquitty::processBytes(session.term.get(), "\x1b]133;A\x07Password: \x1b]133;B\x07");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    // The pane is shown now: this frame is the client's checkpoint, which
    // carries no echo context. The updates after it do.
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    Screen::deliver(d, index, client);

    // Publishes a due frame and carries frames, as the daemon's loop does,
    // until the client's echo context says what is wanted or 500 ms pass: an
    // update waits out the host's update interval.
    const auto deliverUntil = [&](const auto& isWanted) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < end) {
            if (session.isFrameDue)
                Screen::publish(d, session);
            Screen::deliver(d, index, client);
            if (isWanted(client.connection().echoContext(1)))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    Ubiquitty::processBytes(session.term.get(), "*");
    session.isFrameDue = true;
    CHECK_MSG(deliverUntil([](const std::optional<Sync::EchoContext>& held) { return held.has_value(); }), "the client holds no echo context");
    std::optional<Sync::EchoContext> context = client.connection().echoContext(1);
    if (context) {
        CHECK_MSG(context->flags.contains(Sync::EchoFlag::SecureEntry), "echo off was not reported as secure entry");
        CHECK_MSG(context->flags.contains(Sync::EchoFlag::AtPrompt), "the shell's prompt was not reported");
        CHECK_MSG(context->interruptCharacters.interrupt == std::optional<std::uint8_t> { 0x03 }, "the interrupt character was not the line discipline's");
        CHECK_MSG(context->interruptCharacters.quit == std::optional<std::uint8_t> { 0x1c }, "the quit character was not the line discipline's");
        CHECK_MSG(!context->interruptCharacters.suspend.has_value(), "a disabled suspend character was reported");
    }

    // Echo back on, and the command runs.
    pty.facts.discipline->isEchoEnabled = true;
    Ubiquitty::processBytes(session.term.get(), "\r\n\x1b]133;C\x07");
    session.isFrameDue = true;
    (void)deliverUntil([](const std::optional<Sync::EchoContext>& held) { return held && !held->flags.contains(Sync::EchoFlag::SecureEntry); });
    context = client.connection().echoContext(1);
    CHECK_MSG(context && !context->flags.contains(Sync::EchoFlag::SecureEntry), "echo back on was still reported as secure entry");
    CHECK_MSG(context && !context->flags.contains(Sync::EchoFlag::AtPrompt), "a running command was reported as the prompt");

    // Input written under facts that did not move takes no frame: a flood of
    // keys is not a flood of frames.
    CHECK_MSG(!session.isFrameDue, "premise: a frame was still due");
    for (int keys = 0; keys < 64; ++keys) {
        const std::array<std::uint8_t, 1> typed { 'k' };
        CHECK(client.sendRaw(typed).has_value());
        Screen::receive(d, index, client.takeStream(Screen::now()));
    }
    CHECK_MSG(pty.written.ends_with("kkkk"), "premise: the keys were not written");
    CHECK_MSG(!session.isFrameDue, "input under unchanged echo facts took a frame");

    // The program turns echo off and writes nothing, as a password prompt
    // that printed itself already does. The client's first key is written,
    // and the context it is told says so before any output.
    pty.facts.discipline->isEchoEnabled = false;
    const std::array<std::uint8_t, 1> key { 's' };
    CHECK(client.sendRaw(key).has_value());
    CHECK_MSG(deliverUntil([](const std::optional<Sync::EchoContext>& held) { return held && held->flags.contains(Sync::EchoFlag::SecureEntry); }), "echo turned off with no output was never reported");

    if (failures == 0)
        std::printf("echo_context_facts: each frame's echo context carries the PTY's echo and signal characters and the shell's prompt\n");
    return failures == 0 ? 0 : 1;
}
