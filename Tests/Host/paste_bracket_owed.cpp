// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/paste_bracket_owed.cpp — a paste whose client is gone for good
// still closes its bracket (Docs/sync.md §6.7), so the program is not left
// in paste mode.
//
// The program asked for bracketed paste. A client pastes; its first piece is
// written, the bracket opened; then its transport goes. Inside the grace the
// attachment is Lost, not freed, and a resume continues the paste, so the
// bracket stays open.
// Past the grace the daemon's loop frees the attachment and writes the end
// marker.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

int main()
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Ubiquitty::processBytes(session.term.get(), "\x1b[?2004h$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::setTokenID(d, index, "paster");
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    CHECK_MSG(client.pane() != nullptr, "premise: the client was not bound to the pane");

    // One piece of the paste reaches the daemon; the rest never will.
    CHECK(client.sendPaste(std::string(100 * 1024, 'a')).has_value());
    Screen::receive(d, index, client.takeStream(Screen::now()));
    CHECK_MSG(pty.written.starts_with("\x1b[200~a"), "premise: the paste's first piece was not written under its bracket");
    CHECK_MSG(!pty.written.ends_with("\x1b[201~"), "premise: the bracket closed before the paste ended");
    const std::uint64_t droppedAt = Screen::now();
    Screen::drop(d, index);

    // Inside the grace the attachment is Lost, and a resume continues the
    // paste.
    const std::uint64_t grace = Sync::measuredResumePolicy().lostGraceMicroseconds;
    Screen::reapAttachments(d, droppedAt + grace / 2);
    CHECK_MSG(!pty.written.ends_with("\x1b[201~"), "the bracket closed while the attachment could still finish the paste");

    // Past it, the bracket is closed. The repair is queued on the stream ahead
    // of anything written later, and goes out when the loop drains it.
    Screen::reapAttachments(d, Screen::now() + grace + 1'000'000);
    (void)pty.drainPendingWrite();
    CHECK_MSG(pty.written.ends_with("\x1b[201~"), "the bracket a gone client opened was left open");
    CHECK_MSG(pty.written.size() < 100 * 1024, "more of the paste was written than reached the daemon");

    if (failures == 0)
        std::printf("paste_bracket_owed: a paste whose client is gone keeps its bracket open through the grace and has it closed past it\n");
    return failures == 0 ? 0 : 1;
}
