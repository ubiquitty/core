// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A re-homed session keeps its [security] sequence gate.
//
// Local handoff moves parsing from the window into the daemon, which reads no
// config and mints its Terminal from the share's whole-terminal body; the gate
// therefore rides in that body, and the adopted Terminal holds it before it
// parses a byte. Two adopted tenants hold two policies: the daemon is one
// process, and the gate is the Terminal's, not the process's.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

struct RecordingClipboard final : Ubiquitty::ClipboardClient {
    std::string written;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> bytes) override { written.assign(bytes.data(), bytes.size()); }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char>) override { return 0; }
};

constexpr const char* osc52WriteHi = "\033]52;c;aGk=\033\\";

// What a window shares: one whole terminal body, captured as the share path
// captures it, carrying the window's gate in its host-state section.
std::expected<Ubiquitty::Protocol::Sync::HandoverBytes, Ubiquitty::Session::ShareEncoding> shareBodyFor(Ubiquitty::Terminal& window)
{
    Ubiquitty::resize(window, 24, 80);
    return Ubiquitty::Session::captureShareTerminal(window, Ubiquitty::Protocol::Local::maxAdoptBodyBytes);
}

// The terminal the daemon mints from a share's body, under its own limits and
// with no policy of its own: what it holds of the gate is what the body carried.
std::unique_ptr<Ubiquitty::Terminal> adopted(const std::expected<Ubiquitty::Protocol::Sync::HandoverBytes, Ubiquitty::Session::ShareEncoding>& body, RecordingClipboard& clipboard)
{
    if (!body)
        return nullptr;
    auto restored = Ubiquitty::Protocol::Sync::decodeHandoverTerminal(body->view(), Ubiquitty::Session::localShareTerminalLimits(Ubiquitty::Protocol::Local::maxAdoptBodyBytes));
    if (!restored || !restored->terminal)
        return nullptr;
    restored->terminal->clipboardClient = &clipboard;
    restored->terminal->isOSC52Allowed = true;
    return std::move(restored->terminal);
}

} // namespace

TEST(anAdoptedSessionStillRefusesTheOSCItsWindowDenied)
{
    Ubiquitty::Terminal window;
    Ubiquitty::hardReset(window);
    const int deny[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(window, deny, 1));

    // Clients outlive the terminals that point at them.
    RecordingClipboard clipboard;
    const auto daemonTerminal = adopted(shareBodyFor(window), clipboard);
    REQUIRE(daemonTerminal);

    CHECK(!Ubiquitty::OSC::oscAllowed(*daemonTerminal, 52));
    feed(*daemonTerminal, osc52WriteHi);
    CHECK(clipboard.written.empty());

    // And a window that denied nothing re-homes into a session that denies nothing.
    Ubiquitty::Terminal openWindow;
    Ubiquitty::hardReset(openWindow);
    RecordingClipboard openClipboard;
    const auto openDaemonTerminal = adopted(shareBodyFor(openWindow), openClipboard);
    REQUIRE(openDaemonTerminal);
    feed(*openDaemonTerminal, osc52WriteHi);
    CHECK(openClipboard.written == "hi");
}

TEST(anAdoptedSessionStillRefusesTheDCSItsWindowDenied)
{
    Ubiquitty::Terminal window;
    Ubiquitty::hardReset(window);
    const Ubiquitty::OSC::DCSKind deny[] = { Ubiquitty::OSC::DCSKind::DECRQSS };
    CHECK(Ubiquitty::OSC::setDCSDenyList(window, deny, 1));

    // Clients outlive the terminals that point at them.
    RecordingClipboard clipboard;
    const auto daemonTerminal = adopted(shareBodyFor(window), clipboard);
    REQUIRE(daemonTerminal);

    char outputBuffer[256];
    feed(*daemonTerminal, "\033P$qr\033\\");
    CHECK(drain(*daemonTerminal, outputBuffer) == 0);
}

TEST(twoDaemonTenantsHoldTwoPolicies)
{
    Ubiquitty::Terminal hardenedWindow;
    Ubiquitty::Terminal permissiveWindow;
    Ubiquitty::hardReset(hardenedWindow);
    Ubiquitty::hardReset(permissiveWindow);
    const int deny[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(hardenedWindow, deny, 1));
    const int allow[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCAllowList(permissiveWindow, allow, 1));

    RecordingClipboard hardenedClipboard;
    RecordingClipboard permissiveClipboard;
    const auto hardenedTenant = adopted(shareBodyFor(hardenedWindow), hardenedClipboard);
    const auto permissiveTenant = adopted(shareBodyFor(permissiveWindow), permissiveClipboard);
    REQUIRE(hardenedTenant && permissiveTenant);

    feed(*hardenedTenant, osc52WriteHi);
    feed(*permissiveTenant, osc52WriteHi);
    CHECK(hardenedClipboard.written.empty());
    CHECK(permissiveClipboard.written == "hi");

    // The permissive tenant's exclusive allow list of {52} shuts its titles;
    // the hardened tenant, with no allow list, still takes one.
    feed(*hardenedTenant, "\033]2;hardened\033\\");
    feed(*permissiveTenant, "\033]2;permissive\033\\");
    CHECK(hardenedTenant->titleUTF8 == "hardened");
    CHECK(permissiveTenant->titleUTF8 != "permissive");
}

UBIQUITTY_TEST_MAIN()
