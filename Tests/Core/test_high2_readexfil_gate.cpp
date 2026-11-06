// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

// test_high2_readexfil_gate — the respond capability gate, driven
// through the REAL HostCapabilityPolicy tier table.
//
// respond (Source/Core/VT/VT.cpp) is the single chokepoint for every
// host-facing VT reply. Each reply is tagged with the precise capability it
// reveals; on a non-Allow decision for (capability × currentCapabilityOrigin)
// the reply is SUPPRESSED — never queued, so the exfil bytes never reach the
// host. This test pins the security contract end to end:
//
//   - ReadExfil replies (DECRQCRA screen-checksum → ScreenContentRead;
//     XTWINOPS 21t title → WindowTitleRead) are SUPPRESSED at origin=Remote
//     (the real tier table maps ReadExfil→Remote=Deny). This is the core
//     fails-without-it: drop the gate and the exfil bytes appear.
//   - Benign feature/cursor reads (DA / DSR-6 / DECRQM) are still EMITTED at
//     origin=Remote (Benign→Remote=Allow) — proves the gate does not over-block
//     and would catch a mis-tiering of, e.g., DA back into ReadExfil-Deny.
//   - The same DECRQCRA at origin=Local is EMITTED (Local=Allow) — proves the
//     suppression is origin-gated, not unconditional.
//
// The policy is the production Ubiquitty::HostCapabilityPolicy (header-only:
// tierOf + defaultDecision are constexpr in HostCapabilityPolicy.h), so the
// real per-origin tier table is what's under test, not a fake that re-states it.

#include "Core/TerminalOperations.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdio>
#include <cstring>

// Drain the host-facing reply queue into out (NUL-terminated); returns length.

// Install the production policy at a given origin, feed one VT query, and report
// whether respond emitted anything (true = reply queued, false = suppressed).
static bool queryEmits(const char* query, Ubiquitty::CapabilityOrigin origin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    Ubiquitty::HostCapabilityPolicy policy; // real tier table, no grant store
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = origin;
    feed(t, query);
    char outputBuffer[512];
    int n = drain(t, outputBuffer);
    t.capabilityPolicy = nullptr; // clear the CheckedPtr before policy destructs
    return n > 0;
}

// VT queries, each tagged with the capability its reply reveals.
static const char* decrqcra = "\033[12;0;1;1;1;1*y"; // ScreenContentRead (ReadExfil)
static const char* xtwinops21t = "\033[21t"; //          WindowTitleRead  (ReadExfil)
static const char* primaryDA = "\033[c"; //              DeviceAttributesRead (Benign)
static const char* dsr6 = "\033[6n"; //                  CursorStateRead  (Benign)
static const char* decrqm = "\033[?80$p"; //             ModeStateRead    (Benign)

// ReadExfil at Remote → SUPPRESSED. The core fails-without-it: with the gate
// removed (or ReadExfil mis-tiered to Allow) these replies reappear.
TEST(testReadexfilSuppressedAtRemote)
{
    CHECK(!queryEmits(decrqcra, Ubiquitty::CapabilityOrigin::Remote)); // ScreenContentRead
    CHECK(!queryEmits(xtwinops21t, Ubiquitty::CapabilityOrigin::Remote)); // WindowTitleRead
}

// Benign feature/cursor reads at Remote → still EMITTED (Benign→Remote=Allow).
// Programs BLOCK on these for capability detection, so over-blocking would hang
// vim/tmux on a teleport-viewed session. A failure here flags a mis-tiering of
// a feature read into a Deny tier.
TEST(testBenignReadsEmittedAtRemote)
{
    CHECK(queryEmits(primaryDA, Ubiquitty::CapabilityOrigin::Remote)); // DeviceAttributesRead
    CHECK(queryEmits(dsr6, Ubiquitty::CapabilityOrigin::Remote)); // CursorStateRead
    CHECK(queryEmits(decrqm, Ubiquitty::CapabilityOrigin::Remote)); // ModeStateRead
}

// CONTRAST: the same DECRQCRA at origin=Local is EMITTED (Local=Allow). Proves
// the suppression is keyed on the principal's origin, not unconditional — a
// local program reading its own screen is fine.
TEST(testReadexfilEmittedAtLocal)
{
    CHECK(queryEmits(decrqcra, Ubiquitty::CapabilityOrigin::Local)); // ScreenContentRead, Local=Allow
    CHECK(queryEmits(xtwinops21t, Ubiquitty::CapabilityOrigin::Local)); // WindowTitleRead, Local=Allow
}

// ReadExfil at Agent -> SUPPRESSED, and Agent is the origin that ships.
//
// Remote was the covered arm, and it is the one nothing arrives on: a browser
// client reaches the daemon through the web proxy and is stamped Agent,
// because Daemon::originOf returns Local only for a kernel-verified peer named
// ubiquitty-host or Ubiquitty. So the suppression that decides what a real
// client can read was proven only at an origin no real client has.
//
// Run against the daemon and a browser, esctest2 splits exactly at this tier
// boundary: every case reading the screen goes through DECRQCRA and times out,
// every case reading a mode answers.
TEST(testReadexfilSuppressedAtAgent)
{
    CHECK(!queryEmits(decrqcra, Ubiquitty::CapabilityOrigin::Agent)); // ScreenContentRead
    CHECK(!queryEmits(xtwinops21t, Ubiquitty::CapabilityOrigin::Agent)); // WindowTitleRead
}

// Benign reads at Agent still answer, so the case above fails for the tier
// rather than for the origin refusing everything.
TEST(testBenignReadsEmittedAtAgent)
{
    CHECK(queryEmits(primaryDA, Ubiquitty::CapabilityOrigin::Agent)); // DeviceAttributesRead
    CHECK(queryEmits(dsr6, Ubiquitty::CapabilityOrigin::Agent)); // CursorStateRead
    CHECK(queryEmits(decrqm, Ubiquitty::CapabilityOrigin::Agent)); // ModeStateRead
}

// Agent decides Prompt where Remote decides Deny, and with no grant store both
// suppress. The observable is identical, so the cases above cannot tell them
// apart — asserting the decision is what keeps the two origins distinguishable.
// Silently collapsing Agent onto Deny would lose the grant path a prompt exists
// to offer, and every emission case here would still pass.
TEST(testAgentPromptsWhereRemoteDenies)
{
    using Ubiquitty::CapabilityDecision;
    using Ubiquitty::CapabilityOrigin;
    using Ubiquitty::CapabilityTier;
    CHECK(Ubiquitty::defaultDecision(CapabilityTier::ReadExfil, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);
    CHECK(Ubiquitty::defaultDecision(CapabilityTier::ReadExfil, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(Ubiquitty::defaultDecision(CapabilityTier::ReadExfil, CapabilityOrigin::Local) == CapabilityDecision::Allow);
}

UBIQUITTY_TEST_MAIN()
