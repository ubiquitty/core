// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/input_settled_echo.cpp — a frame the daemon sends never reports
// input settled before it holds the echo of a program that echoed within E
// (Docs/sync.md §6.7). The host twin of test_sync_input's echo case, on a real
// PTY.
//
// The program is this binary run with --echo-peer: it reads a byte, sleeps
// 30 ms, echoes it, and appends the monotonic time its write returned to a
// stamp file. A client attached over Sync with the Drive scope types one key
// at a time. The loop's steps run by hand on real time: the PTY is drained
// when readable, input due to settle is checked against a quiet PTY, frames
// are published and sent.
//
// Every key settles, and no report of it settled arrives sooner than E after
// it was sent. For a key the program echoed sooner than E after it was sent,
// every report of it settled the client holds comes with its echo. A key
// echoed later is outside that guarantee, since E is a timer: the next key
// is typed, and the test fails if the program echoed none of them within E.
// Friend seam, the QUIC-off host build, as echo_one_write.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

constexpr std::string_view echoPeerFlag = "--echo-peer";

// The program under the pane. One process serves every key, so an echo waits
// on its own sleep and scheduling, never on another program starting.
int runEchoPeer(const char* stampPath)
{
    termios mode {};
    if (::tcgetattr(STDIN_FILENO, &mode) != 0)
        return 1;
    mode.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &mode) != 0)
        return 1;
    std::ofstream stamps(stampPath, std::ios::app);
    if (!stamps)
        return 1;
    char byte = 0;
    while (::read(STDIN_FILENO, &byte, 1) == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (::write(STDOUT_FILENO, &byte, 1) != 1)
            return 1;
        stamps << Screen::now() << std::endl;
    }
    return 0;
}

std::vector<std::uint64_t> readEchoStamps(const std::filesystem::path& path)
{
    std::vector<std::uint64_t> stamps;
    std::ifstream file(path);
    std::uint64_t stamp = 0;
    while (file >> stamp)
        stamps.push_back(stamp);
    return stamps;
}

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

int main(int argc, char** argv)
{
    if (argc == 3 && argv[1] == echoPeerFlag)
        return runEchoPeer(argv[2]);

    std::error_code error;
    const std::filesystem::path self = argc > 0 ? std::filesystem::canonical(argv[0], error) : std::filesystem::path {};
    CHECK_MSG(!self.empty() && !error, "premise: this binary's own path did not resolve");
    std::array<char, 40> directory { "/tmp/ubiquitty-settled-echo.XXXXXX" };
    const bool hasDirectory = ::mkdtemp(directory.data()) != nullptr;
    CHECK_MSG(hasDirectory, "premise: the stamp directory was not made");
    if (self.empty() || error || !hasDirectory)
        return UBIQUITTY_TEST_SUMMARY();
    const std::filesystem::path stampDirectory(directory.data());
    const std::filesystem::path stampPath = stampDirectory / "echoes";
    const auto finish = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(stampDirectory, ignored);
        return UBIQUITTY_TEST_SUMMARY();
    };

    Daemon d;
    const std::array<std::string, 3> command { self.string(), std::string(echoPeerFlag), stampPath.string() };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return finish();
    const std::size_t index = Screen::attach(d, session->id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
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
    // Let the program start and settle into its read.
    pump(std::chrono::milliseconds(300), [] { });
    CHECK_MSG(client.pane() != nullptr, "premise: the client was not bound to the pane");
    if (!client.pane())
        return finish();

    // The daemon stamps a key written when it receives it, after `sentAt`, so
    // a read that settles the key begins at least E after `sentAt`.
    const std::uint64_t echoSettle = Ubiquitty::Protocol::Sync::measuredInputPolicy().echoSettleMicroseconds;
    constexpr std::string_view keys = "ZYXWVUTS";
    std::uint64_t inputSeq = 0;
    bool wasEchoedWithinSettle = false;
    for (const char key : keys) {
        ++inputSeq;
        CHECK_MSG(!firstRowHolds(*client.pane(), key), "premise: the screen held the key before it was typed");
        const std::array<std::uint8_t, 1> typed { static_cast<std::uint8_t>(key) };
        const std::uint64_t sentAt = Screen::now();
        CHECK_MSG(client.sendRaw(typed).has_value(), "premise: the client did not send the key");
        Screen::receive(d, index, client.takeStream(sentAt));
        std::uint64_t firstSettledAt = 0;
        int settledWithoutEcho = 0;
        pump(std::chrono::milliseconds(400), [&] {
            if (client.connection().receipt(1).inputSettled < inputSeq)
                return;
            if (!firstSettledAt)
                firstSettledAt = Screen::now();
            if (!firstRowHolds(*client.pane(), key))
                ++settledWithoutEcho;
        });
        CHECK_MSG(firstSettledAt != 0, "the key never settled");
        CHECK_MSG(client.connection().receipt(1).inputWritten == inputSeq, "the key was not reported written");
        CHECK_MSG(firstSettledAt == 0 || firstSettledAt >= sentAt + echoSettle, "the client held a report of the key settled sooner than E after it was sent");
        const std::vector<std::uint64_t> echoes = readEchoStamps(stampPath);
        CHECK_MSG(echoes.size() == inputSeq, "premise: the program did not stamp the key's echo");
        if (echoes.size() != inputSeq)
            break;
        if (echoes.back() < sentAt + echoSettle) {
            CHECK_MSG(settledWithoutEcho == 0, "the client held a report of the key settled before its echo");
            wasEchoedWithinSettle = true;
            break;
        }
        std::printf("input_settled_echo: '%c' was echoed %llu ms after it was sent, past E, so the next key is typed\n", key,
            static_cast<unsigned long long>((echoes.back() - sentAt) / 1000));
    }
    CHECK_MSG(wasEchoedWithinSettle, "the program echoed none of the keys within E");

    std::printf("input_settled_echo: a key echoed within E settled only with its echo in the picture\n");
    return finish();
}
