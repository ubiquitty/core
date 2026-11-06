// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The per-OSC / per-DCS sequence gate and the OSC 52 clipboard policy are
// per-terminal state.
//
// Two terminals in one process hold two policies: a code denied on one is
// still honoured on the other, and the gate seeded from one config reaches
// only the terminal it was seeded on. The gate survives a hard reset, since a
// program that issues RIS must not shed the operator's [security] policy. The
// clipboard policy takes the same route: two terminals seeded from divergent
// configs hold divergent write gates, and the write lands on one alone.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"
#include <array>

#include "Core/OSC/Dispatch.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <span>
#include <string>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

// A clipboard that records what OSC 52 wrote to it, so a gated write is
// observable as a write that never arrived.
struct RecordingClipboard final : Ubiquitty::ClipboardClient {
    std::string written;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> bytes) override { written.assign(bytes.data(), bytes.size()); }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char>) override { return 0; }
};

// OSC 52 carrying base64("hi") to the clipboard selection.
constexpr const char* osc52WriteHi = "\033]52;c;aGk=\033\\";

void openWithClipboard(Ubiquitty::Terminal& t, RecordingClipboard& clipboard)
{
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clipboard;
    t.isOSC52Allowed = true;
}

} // namespace

TEST(anOSC52DeniedOnOneTerminalStillWritesFromAnotherInTheSameProcess)
{
    // Clients outlive the terminals that point at them.
    RecordingClipboard hardenedClipboard;
    RecordingClipboard permissiveClipboard;
    Ubiquitty::Terminal hardened;
    Ubiquitty::Terminal permissive;
    openWithClipboard(hardened, hardenedClipboard);
    openWithClipboard(permissive, permissiveClipboard);

    const int deny[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(hardened, deny, 1));
    CHECK(!Ubiquitty::OSC::oscAllowed(hardened, 52));
    CHECK(Ubiquitty::OSC::oscAllowed(permissive, 52));

    feed(hardened, osc52WriteHi);
    feed(permissive, osc52WriteHi);
    CHECK(hardenedClipboard.written.empty());
    CHECK(permissiveClipboard.written == "hi");
}

TEST(aDCSDeniedOnOneTerminalStillRepliesFromAnother)
{
    Ubiquitty::Terminal hardened;
    Ubiquitty::Terminal permissive;
    Ubiquitty::hardReset(hardened);
    Ubiquitty::hardReset(permissive);

    const Ubiquitty::OSC::DCSKind deny[] = { Ubiquitty::OSC::DCSKind::DECRQSS };
    CHECK(Ubiquitty::OSC::setDCSDenyList(hardened, deny, 1));

    char outputBuffer[256];
    feed(hardened, "\033P$qr\033\\");
    CHECK(drain(hardened, outputBuffer) == 0);
    feed(permissive, "\033P$qr\033\\");
    CHECK(drain(permissive, outputBuffer) > 0);
}

TEST(theSeedInstallsTheConfiguredListsOnTheSeededTerminalAlone)
{
    Ubiquitty::Terminal seeded;
    Ubiquitty::Terminal other;
    Ubiquitty::hardReset(seeded);
    Ubiquitty::hardReset(other);

    Ubiquitty::Preferences::Values config;
    config.oscDeny = "52, 7";
    config.dcsDeny = "decrqss";
    config.apcDeny = "G";
    Ubiquitty::Runtime::seedSequenceGate(seeded, config);
    CHECK(!Ubiquitty::OSC::apcAllowed(seeded, 'G'));
    CHECK(Ubiquitty::OSC::apcAllowed(other, 'G'));

    CHECK(!Ubiquitty::OSC::oscAllowed(seeded, 52));
    CHECK(!Ubiquitty::OSC::oscAllowed(seeded, 7));
    CHECK(Ubiquitty::OSC::oscAllowed(seeded, 2));
    CHECK(!Ubiquitty::OSC::dcsAllowed(seeded, Ubiquitty::OSC::DCSKind::DECRQSS));
    CHECK(Ubiquitty::OSC::dcsAllowed(seeded, Ubiquitty::OSC::DCSKind::Sixel));
    CHECK(Ubiquitty::OSC::oscAllowed(other, 52));
    CHECK(Ubiquitty::OSC::dcsAllowed(other, Ubiquitty::OSC::DCSKind::DECRQSS));

    // Re-seeding from a config with no lists returns the terminal to the
    // pass-everything default: the seed carries the whole set, never an addition.
    Ubiquitty::Runtime::seedSequenceGate(seeded, Ubiquitty::Preferences::Values {});
    CHECK(Ubiquitty::OSC::oscAllowed(seeded, 52));
    CHECK(Ubiquitty::OSC::dcsAllowed(seeded, Ubiquitty::OSC::DCSKind::DECRQSS));
    CHECK(Ubiquitty::OSC::apcAllowed(seeded, 'G'));
}

TEST(theGateSurvivesAHardReset)
{
    RecordingClipboard clipboard;
    Ubiquitty::Terminal t;
    openWithClipboard(t, clipboard);
    const int deny[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, deny, 1));

    feed(t, "\033c");
    t.clipboardClient = &clipboard;
    t.isOSC52Allowed = true;
    CHECK(!Ubiquitty::OSC::oscAllowed(t, 52));
    feed(t, osc52WriteHi);
    CHECK(clipboard.written.empty());
}

TEST(anOSC52PolicySeededFromOneConfigReachesTheSeededTerminalAlone)
{
    RecordingClipboard hardenedClipboard;
    RecordingClipboard permissiveClipboard;
    Ubiquitty::Terminal hardened;
    Ubiquitty::Terminal permissive;
    openWithClipboard(hardened, hardenedClipboard);
    openWithClipboard(permissive, permissiveClipboard);

    Ubiquitty::Preferences::Values denying;
    denying.clipboardWrite = "deny";
    denying.clipboardRead = "deny";
    Ubiquitty::Preferences::Values allowing;
    allowing.clipboardWrite = "allow";
    allowing.clipboardRead = "allow";
    Ubiquitty::Runtime::seedOSC52Policy(hardened, denying);
    Ubiquitty::Runtime::seedOSC52Policy(permissive, allowing);

    CHECK(!Ubiquitty::OSC::osc52WriteAllowed(hardened));
    CHECK(Ubiquitty::OSC::osc52WriteAllowed(permissive));
    CHECK(hardened.osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Deny);
    CHECK(permissive.osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Allow);

    feed(hardened, osc52WriteHi);
    feed(permissive, osc52WriteHi);
    CHECK(hardenedClipboard.written.empty());
    CHECK(permissiveClipboard.written == "hi");

    // Re-seeding from a config with no keys returns the terminal to the
    // unset-key defaults: the seed carries the whole pair, never an addition.
    Ubiquitty::Runtime::seedOSC52Policy(hardened, Ubiquitty::Preferences::Values {});
    CHECK(Ubiquitty::OSC::osc52WriteAllowed(hardened));
    CHECK(hardened.osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Prompt);
}

TEST(anOverCapacityDenyListDeniesAtLeastWhatTruncationWouldHave)
{
    // An over-capacity deny closes its whole dimension instead of truncating
    // open. Everything that fit is denied, everything past the cap is denied
    // with it, and so is everything nobody named — all while the overflow
    // stands. A fitting reinstall clears it.
    //
    // A setter that returns early on overflow, before assigning the count,
    // leaves the count at its previous value — zero on a fresh terminal — so a
    // deny list too large to fit denies nothing and permits every code it
    // names: the most permissive outcome available, from a branch meant to fail
    // closed.
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    std::array<int, Ubiquitty::OSC::oscListCapacity + 8> deny {};
    for (std::size_t i = 0; i < deny.size(); ++i)
        deny[i] = 900 + static_cast<int>(i);

    CHECK(!Ubiquitty::OSC::setOSCDenyList(terminal, deny.data(), static_cast<int>(deny.size())));

    // Everything that fit is denied; a count left at zero would allow it.
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, deny[0]));
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, deny[Ubiquitty::OSC::oscListCapacity - 1]));
    // What did not fit is denied too — that is the closed dimension, not a
    // truncation.
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, deny[Ubiquitty::OSC::oscListCapacity]));
    // And so is a code nobody named, allow list or none.
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, 52));

    // A fitting list lifts the closure: the dimension answers its lists again.
    const int fitting[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(terminal, fitting, 1));
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, 52));
    CHECK(Ubiquitty::OSC::oscAllowed(terminal, 0));
}

TEST(anOverCapacityDCSDenyListAnswersTheSameWay)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    std::array<Ubiquitty::OSC::DCSKind, Ubiquitty::OSC::dcsListCapacity + 2> deny {};
    deny.fill(Ubiquitty::OSC::DCSKind::DECRQSS);

    CHECK(!Ubiquitty::OSC::setDCSDenyList(terminal, deny.data(), static_cast<int>(deny.size())));
    CHECK(!Ubiquitty::OSC::dcsAllowed(terminal, Ubiquitty::OSC::DCSKind::DECRQSS));
    // The closed dimension denies the rest too, named or not.
    CHECK(!Ubiquitty::OSC::dcsAllowed(terminal, Ubiquitty::OSC::DCSKind::Sixel));
}

TEST(anOverCapacityAPCDenyListAnswersTheSameWay)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    std::array<std::uint8_t, Ubiquitty::OSC::apcListCapacity + 1> deny {};
    deny.fill('X');

    CHECK(!Ubiquitty::OSC::setAPCDenyList(terminal, deny));
    CHECK(!Ubiquitty::OSC::apcAllowed(terminal, 'X'));
    // Kitty graphics was never named, and the closed dimension denies it too,
    // allow list included.
    const std::uint8_t allow[] = { 'G' };
    CHECK(Ubiquitty::OSC::setAPCAllowList(terminal, allow));
    CHECK(!Ubiquitty::OSC::apcAllowed(terminal, 'G'));

    const std::uint8_t fitting[] = { 'X' };
    CHECK(Ubiquitty::OSC::setAPCDenyList(terminal, fitting));
    CHECK(Ubiquitty::OSC::apcAllowed(terminal, 'G'));
}

TEST(anOverCapacityConfigAPCDenyClosesTheDimension)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    // Every printable introducer but G, more than the list holds.
    std::string deny;
    for (char introducer = '!'; introducer <= '~'; ++introducer) {
        if (introducer != 'G')
            deny += std::string(1, introducer) + " ";
    }
    Ubiquitty::Preferences::Values config;
    config.apcDeny = deny;
    Ubiquitty::Runtime::seedSequenceGate(terminal, config);
    CHECK(!Ubiquitty::OSC::apcAllowed(terminal, 'G'));

    config.apcDeny = std::string("X");
    Ubiquitty::Runtime::seedSequenceGate(terminal, config);
    CHECK(Ubiquitty::OSC::apcAllowed(terminal, 'G'));
}

// An over-capacity deny in config closes the dimension at the fold: a
// 70-entry osc_deny naming 52 denies 52 — and everything else OSC — instead
// of running truncated open. A fitting config afterwards reopens it, which is
// what makes the closure a loud misconfiguration state rather than a latch.
TEST(anOverCapacityConfigDenyClosesTheDimension)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    std::string deny;
    for (int code = 900; code < 900 + Ubiquitty::OSC::oscListCapacity + 6; ++code)
        deny += std::to_string(code) + ",";
    deny += "52";
    Ubiquitty::Preferences::Values config;
    config.oscDeny = deny;
    Ubiquitty::Runtime::seedSequenceGate(terminal, config);

    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, 52));
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, 0));

    config.oscDeny = std::string("52");
    Ubiquitty::Runtime::seedSequenceGate(terminal, config);
    CHECK(!Ubiquitty::OSC::oscAllowed(terminal, 52));
    CHECK(Ubiquitty::OSC::oscAllowed(terminal, 0));
}

TEST(aListThatFitsAnswersTrue)
{
    // The other half of the ratchet: an answer that is false for everything
    // would pass the two cases above and say nothing.
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    const int deny[] = { 52 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(terminal, deny, 1));
    const int allow[] = { 4 };
    CHECK(Ubiquitty::OSC::setOSCAllowList(terminal, allow, 1));
    const Ubiquitty::OSC::DCSKind dcsDeny[] = { Ubiquitty::OSC::DCSKind::DECRQSS };
    CHECK(Ubiquitty::OSC::setDCSDenyList(terminal, dcsDeny, 1));
    CHECK(Ubiquitty::OSC::setDCSAllowList(terminal, dcsDeny, 1));
}

UBIQUITTY_TEST_MAIN()
