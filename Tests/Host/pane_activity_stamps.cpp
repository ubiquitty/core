// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/pane_activity_stamps.cpp — when a pane last wrote and when
// someone last typed into it, as the daemon stamps them.
//
// A listing says "output 2 min ago" and "typed 1 h ago" from two wall-clock
// instants the host keeps per pane (Session::activity). They are taken per
// batch: a drain pass of the PTY stamps output once however many bytes it
// read, and a pass of client input stamps input once however many keys it
// carried. Typing is what counts as input: raw bytes, keys and pastes. A pointer moving over the pane and a window
// gaining focus go to the program through the same queue and are nobody
// typing.
//
// The clock is the test's (ScreenPlaneAccess::setWallClock), so each stamp is
// compared with the instant it was taken at and the reads are counted.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

std::uint64_t wallNow = 0;
std::uint64_t wallReads = 0;

std::uint64_t testWallClock()
{
    ++wallReads;
    return wallNow;
}

} // namespace

int main()
{
    Daemon d;
    Screen::setWallClock(d, &testWallClock);
    // Prints, then reads nothing: every byte after the first burst is the
    // terminal's echo of what the test types.
    const std::array<std::string, 3> command { "/bin/sh", "-c", "printf 'first burst of output'; sleep 60" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    CHECK_MSG(session->activity.lastOutputUnixMilliseconds == 0 && session->activity.lastInputUnixMilliseconds == 0, "never, before anything happens");

    // Runs drain passes until the output stamp moves, or two seconds pass.
    const auto drainUntilStamped = [&](std::uint64_t stampedBefore) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end && session->activity.lastOutputUnixMilliseconds == stampedBefore) {
            Screen::servicePTY(d, *session);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return session->activity.lastOutputUnixMilliseconds != stampedBefore;
    };

    // 1. Output stamps output, at the pass's instant, and nothing else.
    wallNow = 1'000;
    CHECK_MSG(drainUntilStamped(0), "the program's first output was stamped");
    CHECK(session->activity.lastOutputUnixMilliseconds == 1'000);
    CHECK_MSG(session->activity.lastInputUnixMilliseconds == 0, "output is not input");

    // 2. A pass with nothing to read stamps nothing and reads no clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Screen::servicePTY(d, *session);
    wallNow = 2'000;
    const std::uint64_t readsBeforeIdlePass = wallReads;
    Screen::servicePTY(d, *session);
    CHECK_MSG(session->activity.lastOutputUnixMilliseconds == 1'000 && wallReads == readsBeforeIdlePass, "an idle pass is not output");

    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    const std::size_t index = Screen::attach(d, session->id);
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, "typist");
    Screen::markInteractiveUser(d, index);

    // 3. Typing is stamped once, at the end of the pass that carried it, and
    // is not output.
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    Screen::stampTypedInput(d);
    CHECK_MSG(session->activity.lastInputUnixMilliseconds == 0, "premise: attaching is not typing");
    wallNow = 5'000;
    {
        const std::array<std::uint8_t, 1> key { 'x' };
        for (int count = 0; count < 5; ++count)
            CHECK(client.sendRaw(key).has_value());
        Screen::deliver(d, index, client);
        CHECK_MSG(session->activity.lastInputUnixMilliseconds == 0, "not stamped per key as each is written");
        const std::uint64_t readsBefore = wallReads;
        Screen::stampTypedInput(d);
        CHECK_MSG(session->activity.lastInputUnixMilliseconds == 5'000, "the pass's end stamped the typing");
        CHECK_MSG(wallReads == readsBefore + 1, "five keys in one pass read the clock once");
        Screen::stampTypedInput(d);
        CHECK_MSG(wallReads == readsBefore + 1, "a pass nobody typed in reads no clock");
    }
    // 4. Its echo is output, stamped by the pass that reads it and not before.
    const std::uint64_t outputBeforeEcho = session->activity.lastOutputUnixMilliseconds;
    wallNow = 6'000;
    CHECK_MSG(drainUntilStamped(outputBeforeEcho), "the echo was read");
    CHECK(session->activity.lastOutputUnixMilliseconds == 6'000 && session->activity.lastInputUnixMilliseconds == 5'000);

    // 5. A pointer moving and a window gaining focus are not typing.
    wallNow = 9'000;
    {
        const std::array<Ubiquitty::Protocol::Sync::PointerEvent, 1> motion { Ubiquitty::Protocol::Sync::PointerEvent {} };
        (void)client.sendPointer(motion);
        (void)client.sendFocus(Ubiquitty::Modes::FocusState::Focused);
        Screen::deliver(d, index, client);
        Screen::stampTypedInput(d);
        CHECK_MSG(session->activity.lastInputUnixMilliseconds == 5'000, "pointer motion and focus left the input stamp alone");
    }
    return UBIQUITTY_TEST_SUMMARY();
}
