// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/paste_window_never_holds.cpp — a client that sends input is
// always read (Docs/sync.md §6.7).
//
// A program reads nothing. A client types a line ahead, which waits in the
// PTY's queue for good, then pastes a megabyte: the daemon keeps reading that
// client throughout, the pane holds a window of the paste and no more, and the
// client stays attached; the rest waits with the client for the host's
// receipts. A second client that has sent nothing is read
// throughout as well. When the program reads again, the whole paste is
// written.
//
// A daemon that stops reading every client of a session while its PTY holds
// unwritten input leaves a ^C unread behind a line a flooding program never
// takes, and a client attaching then gets a blank screen.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

int main()
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    // A PTY whose program reads nothing: a small queue fills, and every
    // write past it is refused.
    pty.queueWriteAll();
    pty.capWriteQueue(4'096);
    pty.acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };

    const std::size_t idleIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, idleIndex, driving);
    Screen::setTokenID(d, idleIndex, "idle");
    Screen::markInteractiveUser(d, idleIndex);
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, "paster");
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    CHECK_MSG(!Screen::isInputHeld(d, idleIndex), "premise: a client that has sent nothing was held");

    constexpr std::string_view typedAhead = "echo ahead\r";
    CHECK(client.sendRaw({ reinterpret_cast<const std::uint8_t*>(typedAhead.data()), typedAhead.size() }).has_value());
    Screen::deliver(d, index, client);
    CHECK_MSG(session.stream->hasPendingWrite(), "premise: the line typed ahead did not wait in the PTY's queue");
    CHECK_MSG(!Screen::isInputHeld(d, index) && !Screen::isInputHeld(d, idleIndex), "the daemon stopped reading a client while its PTY held input the program never reads");

    constexpr std::size_t pasteBytes = std::size_t { 1 } << 20;
    const std::size_t window = Sync::measuredInputPolicy().pasteWindowBytes;
    CHECK(client.sendPaste(std::string(pasteBytes, 'p')).has_value());
    bool wasEverHeld = false;
    bool wasPTYAlwaysPending = true;
    std::size_t mostHeld = 0;
    for (int rounds = 0; rounds < 400; ++rounds) {
        Screen::deliver(d, index, client);
        wasEverHeld = wasEverHeld || Screen::isInputHeld(d, index) || Screen::isInputHeld(d, idleIndex);
        wasPTYAlwaysPending = wasPTYAlwaysPending && session.stream->hasPendingWrite();
        mostHeld = std::max(mostHeld, session.screenSource->heldInputBytes());
    }
    CHECK_MSG(!wasEverHeld, "the daemon stopped reading a client while a program read none of a paste");
    CHECK_MSG(wasPTYAlwaysPending, "premise: the PTY's queue emptied while the program read nothing");
    CHECK_MSG(mostHeld >= window / 2, ("premise: the pane never filled toward its window; it held " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(mostHeld <= window + 2 * Sync::clientPastePieceBytes, ("the pane held more of the paste than its window: " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(client.connection().isPasting(1), "premise: the paste left whole into a program that reads nothing");
    CHECK_MSG(Screen::connection(d, index) && Screen::connection(d, index)->state() == Sync::ConnectionState::Attached, "the pasting client was closed");

    // The program reads: the receipts open the window and the rest follows.
    pty.allowWrites();
    pty.capWriteQueue(0);
    for (int rounds = 0; rounds < 4'000 && (client.connection().isPasting(1) || session.screenSource->hasPendingInput() || session.stream->hasPendingWrite()); ++rounds) {
        (void)session.stream->drainPendingWrite();
        session.screenSource->drainInput(Screen::now());
        Screen::deliver(d, index, client);
    }
    CHECK_MSG(!client.connection().isPasting(1), "the paste never finished once the program read");
    const auto pasted = static_cast<std::size_t>(std::ranges::count(pty.written, 'p'));
    CHECK_MSG(pasted == pasteBytes, ("the program was not written the whole paste once: " + std::to_string(pasted) + " of " + std::to_string(pasteBytes)).c_str());

    if (failures == 0)
        std::printf("paste_window_never_holds: a Sync client is read through a paste a program is not reading, the pane holds a window of it, and a client that has sent nothing is never held\n");
    return failures == 0 ? 0 : 1;
}
