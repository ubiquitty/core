// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/interrupt_past_unread_paste.cpp — an interrupt always gets
// through (Docs/sync.md §6.7), under a real PTY.
//
// A shell under a real PTY, its terminal as a login shell leaves it (signals
// on, input discarded at one), traps SIGINT and reads nothing. A client pastes
// 8 MiB of lines into it. The terminal takes what it holds unread and no more, so the host
// holds a window of the paste, in the pane and nowhere else, and the rest
// stays with the client. The client presses Ctrl-C: the paste is given up,
// the trap's mark appears within a bound, the host holds nothing of the paste
// any longer, and the client is still attached.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Keys/Keymap.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

int main()
{
    std::string directoryTemplate = (std::filesystem::temp_directory_path() / "ubq-interrupt-paste-XXXXXX").string();
    if (!::mkdtemp(directoryTemplate.data())) {
        std::printf("interrupt_past_unread_paste: premise: no temporary directory\n");
        return 1;
    }
    const std::filesystem::path directory = directoryTemplate;
    const std::filesystem::path readyPath = directory / "ready";
    const std::filesystem::path markPath = directory / "interrupted";

    // Canonical, signals on, the input queue discarded at a signal: a
    // terminal as a shell leaves it for the command it runs. The shell reads
    // nothing and waits a bounded time, and a hangup ends it.
    const std::string script = "stty sane isig -noflsh; "
                               "trap ': > \"$2\"; exit 0' INT; "
                               ": > \"$1\"; "
                               "i=0; while [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done";
    const std::array<std::string, 6> command { "/bin/sh", "-c", script, "sh", readyPath.string(), markPath.string() };
    Daemon d;
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(readyPath) && std::chrono::steady_clock::now() < readyBy)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_MSG(std::filesystem::exists(readyPath), "premise: the program never set its terminal up");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session->id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);

    // What the host holds of the paste: the pane's unwritten input and what
    // the stream queued for the terminal.
    const auto held = [&] { return session->screenSource->heldInputBytes() + session->stream->pendingWriteBytes(); };
    // One pass of the daemon's loop over this session and client.
    const auto pass = [&] {
        session->stream->drainPendingWrite();
        session->screenSource->drainInput(Screen::now());
        Screen::servicePTY(d, *session);
        Screen::deliver(d, index, client);
    };

    constexpr std::size_t pasteBytes = std::size_t { 8 } << 20;
    const std::size_t window = Sync::measuredInputPolicy().pasteWindowBytes;
    // The window, a piece in flight each way, and the stream's one write.
    const std::size_t slack = 4 * Sync::clientPastePieceBytes + (std::size_t { 64 } << 10);
    // Whole lines: a terminal reading by line stops taking input once it
    // holds one the program has not read, and drops an unfinished line that
    // outgrows it instead.
    std::string text;
    text.reserve(pasteBytes);
    while (text.size() < pasteBytes)
        text += std::string(63, 'p') + "\n";
    CHECK(client.sendPaste(text).has_value());
    std::size_t mostHeld = 0;
    std::size_t mostInStream = 0;
    const auto filledBy = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < filledBy) {
        pass();
        mostHeld = std::max(mostHeld, held());
        mostInStream = std::max(mostInStream, session->stream->pendingWriteBytes());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_MSG(mostHeld >= window / 2, ("premise: the host never held toward a window of the paste; it held " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(mostHeld <= window + slack, ("the host held more of the paste than its window: " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(mostInStream <= slack, ("the stream's queue, which numbers nothing, held " + std::to_string(mostInStream) + " bytes of the paste").c_str());
    CHECK_MSG(client.connection().isPasting(1), "premise: the paste left whole into a program that reads nothing");
    CHECK_MSG(!std::filesystem::exists(markPath), "premise: the program was interrupted before the key");

    const std::array<Sync::KeyEvent, 1> interrupt { Sync::KeyEvent { .key = 0, .modifiers = Ubiquitty::Keys::Keymap::keymapModifierCtrl, .kind = Sync::KeyEventKind::Press, .text = {}, .unshiftedCodePoint = 'c', .shiftedCodePoint = 'c', .baseLayoutCodePoint = 0, .wasModified = true } };
    CHECK(client.sendKeys(interrupt).has_value());
    const auto pressedAt = std::chrono::steady_clock::now();
    const auto interruptedBy = pressedAt + std::chrono::seconds(5);
    while (!std::filesystem::exists(markPath) && std::chrono::steady_clock::now() < interruptedBy) {
        pass();
        mostHeld = std::max(mostHeld, held());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pressedAt).count();
    CHECK_MSG(std::filesystem::exists(markPath), "the interrupt never reached the program behind the paste");
    CHECK_MSG(took <= 1000, ("the interrupt took " + std::to_string(took) + " ms to reach the program").c_str());
    for (int rounds = 0; rounds < 50; ++rounds) {
        pass();
        mostHeld = std::max(mostHeld, held());
    }
    CHECK_MSG(mostHeld <= window + slack, ("the host held more of the paste than its window around the interrupt: " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(session->screenSource->heldInputBytes() == 0, ("the pane still holds " + std::to_string(session->screenSource->heldInputBytes()) + " bytes after the interrupt").c_str());
    CHECK_MSG(!client.connection().isPasting(1), "the client is still sending the paste the interrupt gave up");
    CHECK_MSG(Screen::connection(d, index) && Screen::connection(d, index)->state() == Sync::ConnectionState::Attached, "the client was closed");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0)
        std::printf("interrupt_past_unread_paste: an 8 MiB paste into a program that reads nothing held %zu bytes on the host at most, and Ctrl-C reached the program in %lld ms\n", mostHeld, static_cast<long long>(took));
    return failures == 0 ? 0 : 1;
}
