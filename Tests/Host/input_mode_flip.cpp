// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/input_mode_flip.cpp — the daemon encodes a remote client's key
// under the modes live when it writes it (Docs/sync.md §6.7), the host twin
// of test_remote_input's mode-flip case.
//
// A client attached over Sync with the Drive scope sends Up. The program's
// input is full, so the write is refused and waits. The program then sets
// DECCKM, its input drains, and the key it reads is the application form
// `ESC O A`, not the `ESC [ A` it would have been when the client sent it.
// A client without the Drive scope that sends a key is closed and writes
// nothing. Friend seam, the QUIC-off host build, as echo_one_write.
//

#include "Assertions.h"

#include "Core/Keys/KeyTranslation.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

Ubiquitty::Protocol::Sync::KeyEvent upArrow()
{
    Ubiquitty::Runtime::KeyPress press;
    press.virtualKey = Ubiquitty::Keys::VirtualKey::UpArrow;
    return Ubiquitty::Runtime::keyEventOf(press);
}

} // namespace

int main()
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& program = *stream;
    // The program is not reading: its input queue holds two bytes, and the
    // three the key takes do not fit.
    program.queueWriteAll();
    program.capWriteQueue(2);
    program.acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    Screen::deliver(d, index, client);
    CHECK_MSG(client.pane() != nullptr, "premise: the client was not bound to the pane");

    const std::vector<Ubiquitty::Protocol::Sync::KeyEvent> keys { upArrow() };
    CHECK_MSG(client.sendKeys(keys).has_value(), "premise: the client did not send the key");
    Screen::receive(d, index, client.takeStream(Screen::now()));
    CHECK_MSG(program.written.empty(), ("premise: the refused key was written: " + program.written).c_str());
    CHECK_MSG(session.screenSource->hasPendingInput(), "the refused key was not kept");

    // The program turns on application cursor keys, then reads again.
    Ubiquitty::processBytes(session.term.get(), "\x1b[?1h");
    program.allowWrites();
    program.capWriteQueue(0);
    session.screenSource->drainInput(Screen::now());
    CHECK_MSG(program.written == "\x1bOA", ("the key was not encoded at the write: " + program.written).c_str());
    CHECK_MSG(!session.screenSource->hasPendingInput(), "the key was written but kept");

    // A second client without the Drive scope is closed for sending a key, and
    // nothing of it reaches the program.
    const std::size_t viewer = Screen::attach(d, session.id);
    Screen::grantScope(d, viewer, { Ubiquitty::Protocol::Sync::Scope::Observe });
    Screen::markInteractiveUser(d, viewer);
    UbiquittyTest::ScreenClient watcher(24, 80);
    Screen::greet(d, viewer, watcher);
    Screen::deliver(d, viewer, watcher);
    CHECK_MSG(watcher.sendKeys(keys).has_value(), "premise: the watcher did not send the key");
    Screen::receive(d, viewer, watcher.takeStream(Screen::now()));
    const Ubiquitty::Protocol::Sync::HostConnection* refused = Screen::connection(d, viewer);
    CHECK_MSG(refused && refused->state() == Ubiquitty::Protocol::Sync::ConnectionState::Closed, "a client without Drive sent a key and stayed attached");
    CHECK_MSG(program.written == "\x1bOA", ("a client without Drive wrote to the program: " + program.written).c_str());

    std::printf("input_mode_flip: a refused key was written once, encoded under the modes live at the write\n");
    return UBIQUITTY_TEST_SUMMARY();
}
