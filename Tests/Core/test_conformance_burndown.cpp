// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/VT/VT100Fixture.h"
#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Encodings.h"

#include <cstring>

using namespace Ubiquitty;
using UbiquittyTest::cell;
using UbiquittyTest::drain;
using UbiquittyTest::feed;

static void feedC1(Ubiquitty::Terminal& t, uint8_t byte)
{
    char b = static_cast<char>(byte);
    Ubiquitty::processBytes(&t, std::string_view(&b, 1));
}

// ---------------------------------------------------------------------------
// SPA / EPA — 7-bit and 8-bit, and the 8-bit byte under UTF-8
// ---------------------------------------------------------------------------

TEST(testSevenBitSPAAndEPAGuardTheAreaFromED)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "\033[1;1HX");
    feed(t, "\033V");
    feed(t, "G");
    feed(t, "\033W");
    feed(t, "U");
    const Ubiquitty::Cell* guardedCell = cell(t, 0, 1);
    CHECK(guardedCell && guardedCell->codePoint == 'G');
    CHECK(guardedCell && penIsGuarded(&Ubiquitty::cellPen(*guardedCell)));
    CHECK(t.protectedMode == Ubiquitty::VT::ProtectedMode::ISO);
    feed(t, "\033[H\033[J");
    CHECK(cell(t, 0, 1)->codePoint == 'G');
    CHECK(cell(t, 0, 2)->codePoint == 0);
}

TEST(testEPAWithoutSPAGuardsNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");
    feed(t, "A");
    feed(t, "\033W");
    CHECK(t.protectedMode == Ubiquitty::VT::ProtectedMode::ISO);
    CHECK(!penIsGuarded(&t.screens.active->pen));
    feed(t, "B");
    const Ubiquitty::Cell* plainCell = cell(t, 0, 1);
    CHECK(plainCell && plainCell->codePoint == 'B');
    CHECK(!penIsGuarded(&Ubiquitty::cellPen(*plainCell)));
    feed(t, "\033[H\033[J");
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testEightBitSPAAndEPAGuardUnderLatin1)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\033[H");
    feedC1(t, 0x96);
    CHECK(t.protectedMode == Ubiquitty::VT::ProtectedMode::ISO);
    feed(t, "G");
    const Ubiquitty::Cell* guardedCell = cell(t, 0, 0);
    CHECK(guardedCell && penIsGuarded(&Ubiquitty::cellPen(*guardedCell)));
    feedC1(t, 0x97);
    CHECK(!penIsGuarded(&t.screens.active->pen));
    CHECK(t.protectedMode == Ubiquitty::VT::ProtectedMode::ISO);
    feed(t, "U");
    const Ubiquitty::Cell* unguardedCell = cell(t, 0, 1);
    CHECK(unguardedCell && !penIsGuarded(&Ubiquitty::cellPen(*unguardedCell)));
    feed(t, "\033[H\033[J");
    CHECK(cell(t, 0, 0)->codePoint == 'G');
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testEightBitSPAIsAContinuationByteUnderUTF8)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::UTF8);
    feed(t, "\033[H");
    feedC1(t, 0x96);
    CHECK(t.protectedMode == Ubiquitty::VT::ProtectedMode::Off);
    feed(t, "G");
    bool foundProtected = false;
    for (int c = 0; c < 2; ++c) {
        const Ubiquitty::Cell* candidate = cell(t, 0, c);
        if (candidate && penIsGuarded(&Ubiquitty::cellPen(*candidate)))
            foundProtected = true;
    }
    CHECK(!foundProtected);
}

// ---------------------------------------------------------------------------
// OSC 777 — notify, an unknown extension, malformed payloads, the policy gate
// ---------------------------------------------------------------------------

TEST(testOSC777NotifyRaisesANotification)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;
    feed(t, "\033]777;notify;Title777;Body777\033\\");
    CHECK(shell.notifyCalls == 1);
    CHECK(strcmp(shell.notifyTitle, "Title777") == 0);
    CHECK(strcmp(shell.notifyBody, "Body777") == 0);
    feed(t, "\033]777;notify;OnlyTitle\033\\");
    CHECK(shell.notifyCalls == 2);
    CHECK(strcmp(shell.notifyTitle, "OnlyTitle") == 0);
    t.shellClient = nullptr;
}

TEST(testOSC777UnknownExtensionNeverReachesTheGrid)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;
    feed(t, "\033]777;something;else\033\\");
    CHECK(shell.notifyCalls == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    t.shellClient = nullptr;
}

TEST(testOSC777EmptyAndMalformedPayloadsNotifyNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;
    feed(t, "\033]777;notify;\033\\");
    CHECK(shell.notifyCalls == 0);
    feed(t, "\033]777;notify\033\\");
    CHECK(shell.notifyCalls == 0);
    feed(t, "\033]777;\033\\");
    CHECK(shell.notifyCalls == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    t.shellClient = nullptr;
}

TEST(testOSC777NotifyHonoursTheCapabilityPolicy)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;
    struct DenyPolicy : Ubiquitty::CapabilityPolicy {
        Ubiquitty::CapabilityDecision check(Ubiquitty::Capability c, Ubiquitty::CapabilityOrigin, std::string_view) override
        {
            if (c == Ubiquitty::Capability::DesktopNotification)
                return Ubiquitty::CapabilityDecision::Deny;
            return Ubiquitty::CapabilityDecision::Allow;
        }
    } policy;
    t.capabilityPolicy = &policy;
    feed(t, "\033]777;notify;Title;Body\033\\");
    CHECK(shell.notifyCalls == 0);
    t.capabilityPolicy = nullptr;
    t.shellClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
