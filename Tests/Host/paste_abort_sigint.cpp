// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/paste_abort_sigint.cpp — Ctrl-C during a paste interrupts the
// program, and the program is not left in paste mode (Docs/sync.md §6.7).
//
// A shell under a real PTY traps SIGINT, marking it in one file, while a child
// that ignores it copies what the terminal delivers to another. A client
// pastes; one piece reaches the daemon and is written under the bracket. The
// client presses Ctrl-C: the paste is given up, the daemon writes the
// bracket's end and then the interrupt, and the line discipline turns the
// interrupt into SIGINT. The trap's mark appears, the end bracket is read,
// and the interrupt byte itself is never read as data.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

std::string contentsOf(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

} // namespace

int main()
{
    std::string directoryTemplate = (std::filesystem::temp_directory_path() / "ubq-paste-sigint-XXXXXX").string();
    if (!::mkdtemp(directoryTemplate.data())) {
        std::printf("paste_abort_sigint: premise: no temporary directory\n");
        return 1;
    }
    const std::filesystem::path directory = directoryTemplate;
    const std::filesystem::path readPath = directory / "read";
    const std::filesystem::path readyPath = directory / "ready";
    const std::filesystem::path markPath = directory / "interrupted";

    // Non-canonical so each byte is read as it comes, signals on, and the
    // input queue kept at the signal so the bytes around it are still read.
    // The copy ignores the interrupt and the shell's trap marks it: a reader
    // the signal killed could die holding bytes it had read and not written.
    // The mark goes to a file of its own, since the copy writes a character
    // at a time and a mark in the same file lands inside the end bracket.
    // The shell waits a bounded number of times, and a hangup ends both.
    // The copy reads the terminal through a descriptor saved before it is
    // put in the background: a shell without job control gives a background
    // command /dev/null to read, and dash does so before a `<&0` on the
    // command is applied, which bash does not.
    const std::string script = "stty -icanon -echo isig noflsh; "
                               "trap ': > \"$3\"' INT; "
                               "exec 3<&0; "
                               "(trap '' INT; exec cat -u -v <&3 >> \"$1\") & "
                               ": > \"$2\"; "
                               "for i in 1 2 3 4; do wait; done";
    const std::array<std::string, 7> command { "/bin/sh", "-c", script, "sh", readPath.string(), readyPath.string(), markPath.string() };
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

    // The program asks for bracketed paste.
    Ubiquitty::processBytes(session->term.get(), "\x1b[?2004h");
    Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session->id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);

    // One piece of the paste leaves; then Ctrl-C.
    constexpr std::size_t pasteBytes = 40 * 1024;
    CHECK(client.sendPaste(std::string(pasteBytes, 'a')).has_value());
    Screen::receive(d, index, client.takeStream(Screen::now()));
    const std::array<std::uint8_t, 1> interrupt { 0x03 };
    CHECK(client.sendRaw(interrupt).has_value());
    Screen::receive(d, index, client.takeStream(Screen::now()));

    std::string read;
    const auto settledBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settledBy) {
        // The PTY takes the input as the program reads it: what it refused
        // is written when it is writable again, as the daemon's loop does.
        session->stream->drainPendingWrite();
        session->screenSource->drainInput(Screen::now());
        Screen::service(d);
        read = contentsOf(readPath);
        if (std::filesystem::exists(markPath) && read.contains("^[[201~"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string summary = "; the program read " + std::to_string(read.size()) + " bytes ending \"" + read.substr(read.size() > 24 ? read.size() - 24 : 0) + "\"";
    CHECK_MSG(read.starts_with("^[[200~a"), ("the paste was not read under its bracket" + summary).c_str());
    CHECK_MSG(std::filesystem::exists(markPath), ("the interrupt did not signal the program" + summary).c_str());
    CHECK_MSG(read.contains("^[[201~"), ("the program was left in paste mode" + summary).c_str());
    CHECK_MSG(!read.contains("^C"), "the interrupt was read as data, not acted on by the line discipline");
    CHECK_MSG(read.size() < pasteBytes, "more of the paste was written than left before the interrupt");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0)
        std::printf("paste_abort_sigint: Ctrl-C during a paste closes the bracket and signals the program\n");
    return failures == 0 ? 0 : 1;
}
