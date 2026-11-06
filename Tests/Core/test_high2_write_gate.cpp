// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_high2_write_gate — the WRITE capability gates, driven through the
// REAL HostCapabilityPolicy tier table. The fails-without-it companion to
// test_high2_readexfil_gate: that one pins host-facing REPLIES (read-exfil); this
// one pins grid-escaping WRITES (the side effects the byte stream tries to apply
// to the host).
//
// Each OSC/CSI chokepoint runs `policy->check(cap, currentCapabilityOrigin, …) !=
// Allow → suppress the write` before applying its side effect. The real tier table
// (HostCapabilityPolicy.h) maps:
//   ClipboardWrite, WindowMutate          → HostEscaping → Remote=Deny,   Local=Allow
//   SetWorkingDirectory / SetRemoteHost /
//   SetCurrentFileURL                      → LowRiskInfo  → Remote=Prompt (restricted), Local=Allow
// LowRiskInfo is Allow for Remote by default; these cases are about the gate, so
// they run under a host configured to restrict that tier to Prompt. Prompt is not
// Allow, so at origin=Remote all these writes must not apply; at origin=Local
// they do. We assert the SIDE EFFECT, not a reply:
//
//   - ClipboardWrite (OSC 52 SET + OSC 1337 Copy=) — the paste-injection vector:
//     a fake ClipboardClient records writeClipboard. Remote → never called (no
//     bytes reach the pasteboard); Local → called. Core fails-without-it: drop the
//     gate at the OSC 52 / Copy= site and the Remote write reappears.
//   - SetWorkingDirectory (OSC 7, OSC 1337 CurrentDir=, OSC 633;P;Cwd=) — observed
//     on Terminal::currentDirectory: Remote → stays empty; Local → set.
//   - SetRemoteHost (OSC 1337 RemoteHost=, OSC 7 authority) — Terminal::remoteHost /
//     Terminal::reportedHost: Remote empty / Local set.
//   - SetCurrentFileURL (OSC 6) — Terminal::currentFileURL: Remote empty / Local set.
//   - WindowMutate (XTWINOPS CSI 8 ; rows ; cols t resize, the client-less core
//     path) — Terminal::rows/columns: Remote → unchanged; Local → resized.
//   - WindowMutate (DEC private mode ?30, the rxvt scrollbar hide, whose store
//     is the effect) — Terminal::scrollbarVisible: Remote → stays shown; Local → hidden.
//   - SetCursorShape (OSC 22, the kitty pointer shape, whose stack is the effect
//     the snapshot publishes) — Terminal::mousePointerShapeStackLength: Remote → 0; Local → 1.
//
// The policy is the production Ubiquitty::HostCapabilityPolicy (constexpr tierOf +
// CapabilityDecisionTable), installed exactly as the daemon installs it — so the
// real per-origin tier table is under test, not a fake that re-states it.

#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// A minimal ClipboardClient that records the last write — the observable for the
// ClipboardWrite gate. The daemon's real sink overwrites the host pasteboard; if
// writeClipboard is never called, no bytes reached it.
struct RecordingClipboard final : Ubiquitty::ClipboardClient {
    int writeCount = 0;
    Ubiquitty::ClipboardBuffer lastBuffer {};
    std::string lastData;

    void writeClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<const char> data) override
    {
        ++writeCount;
        lastBuffer = buffer;
        lastData.assign(data.data(), data.size());
    }
};

// Install the production policy at a given origin and run `body(&t)`; clears the
// CheckedPtr before the policy destructs (mirrors test_high2_readexfil_gate).
template<typename Body> static void withTerm(Ubiquitty::CapabilityOrigin origin, Body&& body)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    // Real tier table with Remote's informational tier restricted, no grant store.
    Ubiquitty::HostCapabilityPolicy policy(Ubiquitty::CapabilityDecisionTable {}.replacing(
        Ubiquitty::CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::LowRiskInfo, Ubiquitty::CapabilityDecision::Prompt));
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = origin;
    body(t);
    t.capabilityPolicy = nullptr; // clear the CheckedPtr before policy destructs
}

/// A policy that refuses one capability and allows every other.
///
/// The real tier table puts SetWorkingDirectory and SetRemoteHost in one tier,
/// so no origin separates them. Only a policy that splits them shows which gate
/// a write answers to.
class PolicyRefusingOneCapability final : public Ubiquitty::CapabilityPolicy {
public:
    /// Creates a policy that refuses a single capability.
    ///
    /// - Parameter refused: The capability every check for is denied.
    explicit PolicyRefusingOneCapability(Ubiquitty::Capability refused)
        : m_refused(refused)
    {
    }

    /// Denies the refused capability and allows every other, at any origin.
    ///
    /// - Parameters:
    ///   - capability: What the caller is about to do.
    ///   - origin: Where the request came from; not consulted.
    ///   - detail: The request payload; not consulted.
    /// - Returns: Deny for the refused capability, Allow otherwise.
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin /*origin*/, std::string_view /*detail*/) override
    {
        return capability == m_refused ? Ubiquitty::CapabilityDecision::Deny : Ubiquitty::CapabilityDecision::Allow;
    }

private:
    /// The capability this policy denies.
    Ubiquitty::Capability m_refused;
};

// ---------------------------------------------------------------------------
// ClipboardWrite — OSC 52 SET. "SGVsbG8=" base64-decodes to "Hello". The fake
// records the write; at Remote it must not be called (HostEscaping→Remote=Deny).
// This is the paste-injection vector: a remote staging bytes the user then pastes.
// ---------------------------------------------------------------------------
static const char* osc52SetHello = "\033]52;c;SGVsbG8=\033\\";
// OSC 1337 Copy= — iTerm2's second clipboard-write channel; same gate.
static const char* osc1337Copy = "\033]1337;Copy=:SGVsbG8=\033\\";

TEST(testClipboardWriteSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        RecordingClipboard clip;
        t.clipboardClient = &clip;
        feed(t, osc52SetHello);
        CHECK(clip.writeCount == 0); // OSC 52 SET suppressed — no paste-injection
        feed(t, osc1337Copy);
        CHECK(clip.writeCount == 0); // OSC 1337 Copy= suppressed too (no bypass)
        t.clipboardClient = nullptr;
    });
}

// CONTRAST: the same writes at origin=Local do reach the pasteboard (Local=Allow).
// Proves the suppression is origin-gated, not unconditional, and that the wire
// payload + decode path are otherwise correct.
TEST(testClipboardWriteAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        RecordingClipboard clip;
        t.clipboardClient = &clip;
        feed(t, osc52SetHello);
        CHECK(clip.writeCount == 1); // OSC 52 SET applied
        CHECK(clip.lastBuffer == Ubiquitty::ClipboardBuffer::Clipboard);
        CHECK(clip.lastData == "Hello");
        feed(t, osc1337Copy);
        CHECK(clip.writeCount == 2); // OSC 1337 Copy= applied
        CHECK(clip.lastData == "Hello");
        t.clipboardClient = nullptr;
    });
}

// ---------------------------------------------------------------------------
// SetWorkingDirectory — OSC 7, OSC 1337 CurrentDir=, OSC 633;P;Cwd=. All three
// route to Terminal::currentDirectory. LowRiskInfo→Remote=Prompt (≠Allow) → suppressed.
// ---------------------------------------------------------------------------
static const char* osc7 = "\033]7;file:///tmp/wd7\033\\";
static const char* osc1337Cwd = "\033]1337;CurrentDir=/tmp/wd1337\033\\";
static const char* osc633Cwd = "\033]633;P;Cwd=/tmp/wd633\033\\";

TEST(testSetcwdSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.currentDirectory.empty());
        feed(t, osc7);
        CHECK(t.currentDirectory.empty()); // OSC 7 suppressed
        feed(t, osc1337Cwd);
        CHECK(t.currentDirectory.empty()); // OSC 1337 CurrentDir= suppressed
        feed(t, osc633Cwd);
        CHECK(t.currentDirectory.empty()); // OSC 633;P;Cwd= suppressed
    });
}

TEST(testSetcwdAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, osc7);
        CHECK(t.currentDirectory == "/tmp/wd7"); // OSC 7 applied
        feed(t, osc1337Cwd);
        CHECK(t.currentDirectory == "/tmp/wd1337"); // OSC 1337 applied
        feed(t, osc633Cwd);
        CHECK(t.currentDirectory == "/tmp/wd633"); // OSC 633 applied
    });
}

// ---------------------------------------------------------------------------
// The OSC 7 authority — Terminal::reportedHost. It names the machine the
// session is on, the identity OSC 1337 RemoteHost= writes, so it is recorded
// only where both SetWorkingDirectory (the report) and SetRemoteHost (the
// identity) are allowed. A refused host is neither replaced nor cleared.
// ---------------------------------------------------------------------------
static const char* osc7WithHost = "\033]7;file://evil.example/tmp/wd7\033\\";

TEST(anOSC7HostIsNotRecordedWhereTheWorkingDirectoryIsDenied)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.reportedHost.empty());
        CHECK(!t.currentDirectoryChanged);
        feed(t, osc7WithHost);
        CHECK(t.reportedHost.empty()); // a refused report names no machine
        CHECK(t.currentDirectory.empty());
        CHECK(!t.currentDirectoryChanged); // and raises no edge a consumer acts on
    });
}

// CONTRAST: the same report at origin=Local records the host, so the refusal
// above is origin-gated rather than an unconditional drop of the authority.
TEST(anOSC7HostIsRecordedWhereTheWorkingDirectoryIsAllowed)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, osc7WithHost);
        CHECK(t.reportedHost == "evil.example");
        CHECK(t.currentDirectory == "/tmp/wd7");
        CHECK(t.currentDirectoryChanged);
    });
}

TEST(anOSC7HostRefusedTheRemoteHostKeepsTheRecordedHostAndMovesTheDirectory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    feed(t, "\033]7;file://trusted-host/home/user\033\\"); // no policy: PAL allows Local
    CHECK(t.reportedHost == "trusted-host");
    CHECK(t.reportedHostProcessGroup == 0); // an accepted host waits for the probe after it
    CHECK(!Ubiquitty::retireReportedHostIfStale(t, 100)); // which stamps the program that sent it
    CHECK(t.reportedHostProcessGroup == 100);
    CHECK(t.currentDirectory == "/home/user");

    PolicyRefusingOneCapability policy { Ubiquitty::Capability::SetRemoteHost };
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    feed(t, osc7WithHost);
    CHECK(t.reportedHost == "trusted-host"); // a refused host is not replaced
    CHECK(t.reportedHostProcessGroup == 100); // nor re-stamped: it still retires with the program that sent it
    CHECK(t.currentDirectory == "/tmp/wd7"); // the directory from the same report applies

    feed(t, "\033]7;file:///tmp\033\\");
    CHECK(t.reportedHost == "trusted-host"); // nor cleared by a report that names no host
    CHECK(t.reportedHostProcessGroup == 100);
    CHECK(t.currentDirectory == "/tmp");

    t.capabilityPolicy = nullptr; // clear the CheckedPtr before the policy destructs
}

// The split the tier table cannot make from the other side: SetRemoteHost
// allowed, SetWorkingDirectory refused. The host answers to both gates, so the
// refused report neither replaces nor clears it and raises no edge.
TEST(anOSC7HostIsNotRecordedWhereOnlyTheWorkingDirectoryIsDenied)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    feed(t, "\033]7;file://trusted-host/home/user\033\\"); // no policy: PAL allows Local
    CHECK(t.reportedHost == "trusted-host");
    CHECK(!Ubiquitty::retireReportedHostIfStale(t, 100)); // the probe after it stamps its program
    CHECK(t.reportedHostProcessGroup == 100);
    CHECK(t.currentDirectory == "/home/user");
    t.currentDirectoryChanged = false; // consumed, as the runtime's drain consumes it

    PolicyRefusingOneCapability policy { Ubiquitty::Capability::SetWorkingDirectory };
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    feed(t, osc7WithHost);
    CHECK(t.reportedHost == "trusted-host"); // a refused report does not rename the machine
    CHECK(t.reportedHostProcessGroup == 100); // nor re-stamp the host it left standing
    CHECK(t.currentDirectory == "/home/user");
    CHECK(!t.currentDirectoryChanged); // and raises no edge a consumer acts on

    feed(t, "\033]7;file:///tmp\033\\");
    CHECK(t.reportedHost == "trusted-host"); // nor clears it with a report that names no host
    CHECK(t.reportedHostProcessGroup == 100);
    CHECK(t.currentDirectory == "/home/user");
    CHECK(!t.currentDirectoryChanged);

    t.capabilityPolicy = nullptr; // clear the CheckedPtr before the policy destructs
}

// ---------------------------------------------------------------------------
// SetRemoteHost — OSC 1337 RemoteHost=, routes to Terminal::remoteHost.
// ---------------------------------------------------------------------------
static const char* osc1337RemoteHost = "\033]1337;RemoteHost=user@evil.example\033\\";

TEST(testSetremotehostSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.remoteHost.empty());
        feed(t, osc1337RemoteHost);
        CHECK(t.remoteHost.empty()); // RemoteHost= suppressed (Remote=Prompt)
    });
}

TEST(testSetremotehostAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, osc1337RemoteHost);
        CHECK(t.remoteHost == "user@evil.example"); // applied at Local
    });
}

// ---------------------------------------------------------------------------
// SetCurrentFileURL — OSC 6, routes to Terminal::currentFileURL.
// ---------------------------------------------------------------------------
static const char* osc6 = "\033]6;file:///tmp/doc.txt\033\\";

TEST(testSetfileurlSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.currentFileURL.empty());
        feed(t, osc6);
        CHECK(t.currentFileURL.empty()); // OSC 6 suppressed (Remote=Prompt)
    });
}

TEST(testSetfileurlAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, osc6);
        CHECK(t.currentFileURL == "file:///tmp/doc.txt"); // applied at Local
    });
}

// ---------------------------------------------------------------------------
// WindowMutate — XTWINOPS CSI 8 ; rows ; cols t (host-driven cell resize). With no
// windowClient installed (the core/test path), an Allow resizes the model directly
// (Terminal::rows/columns); a non-Allow skips it. HostEscaping→Remote=Deny. We observe
// rows/columns. (The performWindowOperation move/raise ops route only through a
// windowClient sink, so they're covered by the same gate via the same check — the
// resize path is the one with a client-less, directly-observable side effect.)
// ---------------------------------------------------------------------------
static const char* xtwinopsResize = "\033[8;10;60t"; // resize to 10 rows x 60 cols

TEST(testWindowmutateResizeSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        const int rows0 = t.rows, cols0 = t.columns; // 5 x 40 from withTerm
        feed(t, xtwinopsResize);
        CHECK(t.rows == rows0); // resize suppressed — grid unperturbed
        CHECK(t.columns == cols0);
    });
}

TEST(testWindowmutateResizeAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, xtwinopsResize);
        CHECK(t.rows == 10); // resize applied at Local
        CHECK(t.columns == 60);
    });
}

// ---------------------------------------------------------------------------
// WindowMutate — DEC private mode ?30 (rxvt show/hide scrollbar). The bit is the
// effect: no client is called, both macOS shells read it off the published mode
// snapshot, so the store is the only gate site left. Observed on
// Terminal::scrollbarVisible.
// ---------------------------------------------------------------------------
static const char* hideScrollbar = "\033[?30l";

TEST(testWindowmutateScrollbarHideSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.scrollbarVisible); // hardReset default: shown
        feed(t, hideScrollbar);
        CHECK(t.scrollbarVisible); // refused — a remote cannot take the bar away
    });
}

TEST(testWindowmutateScrollbarHideAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, hideScrollbar);
        CHECK(!t.scrollbarVisible); // applied at Local
    });
}

// ---------------------------------------------------------------------------
// SetCursorShape — OSC 22 (kitty pointer shapes). The stack is what the
// snapshot publishes and both macOS shells read, so the store is the gate;
// the client push keeps its own for the frontends that still take it.
// LowRiskInfo→Remote=Prompt (≠Allow) → the stack stays empty.
// ---------------------------------------------------------------------------
static const char* osc22Pointer = "\033]22;pointer\033\\";

TEST(testSetcursorshapePointerSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        CHECK(t.mousePointerShapeStackLength == 0);
        feed(t, osc22Pointer);
        CHECK(t.mousePointerShapeStackLength == 0); // refused — a remote cannot take the pointer
    });
}

TEST(testSetcursorshapePointerAppliedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, osc22Pointer);
        CHECK(t.mousePointerShapeStackLength == 1); // applied at Local
        CHECK(t.mousePointerShapeStack[0] == Ubiquitty::MousePointerShape::Pointer);
    });
}

UBIQUITTY_TEST_MAIN()
