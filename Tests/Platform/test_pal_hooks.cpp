// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_pal_hooks.cpp — pin Core's per-Ubiquitty::Terminal client routing.
//
// The surviving frontend concerns route through Terminal's CheckedPtr client
// pointers (PAL/TerminalClients.h), not process-wide PAL hooks. This
// test subclasses each client interface, assigns it to a Terminal, drives a
// real OSC / APC dispatch via Terminal::processBytes, and asserts Core called
// the client:
//   - ClipboardClient    (OSC 52 write + query)
//   - ShellClient        (OSC 7 currentDirectory, OSC 9 notification)
//   - FileReadClient     (kitty t=t file transport)
//   - DiagnosticsClient  (unknown-OSC-Ps observer)
//
// Selection-color (OSC 17/19) still routes through the PAL hook surface
// and is pinned here too.
//
// Clients are declared before the Ubiquitty::Terminal so the Terminal (which holds the
// CheckedPtrs) destructs first — CanMakeCheckedPtr traps a client
// outliving a Ubiquitty::Terminal that still references it.

#include "Core/OSC/Dispatch.h"
#include "Core/TerminalOperations.h"
#include "PAL/PAL.h"

#include <cstdio>
#include <cstring>
#include <span>

using namespace Ubiquitty;

// ---- client subclasses -------------------------------------------

struct TestClipboard : ClipboardClient {
    char data[64];
    int length = -1;
    int setCalls = 0;
    void writeClipboard(ClipboardBuffer /*buffer*/, std::span<const char> d) override
    {
        setCalls++;
        length = static_cast<int>(d.size());
        if (!d.empty() && d.size() < sizeof(data))
            std::memcpy(data, d.data(), d.size());
    }
    [[nodiscard]] int readClipboard(ClipboardBuffer /*buffer*/, std::span<char> outputBuffer) override
    {
        const char* s = "Hi";
        int n = 2;
        if (n > static_cast<int>(outputBuffer.size()))
            n = static_cast<int>(outputBuffer.size());
        std::memcpy(outputBuffer.data(), s, static_cast<size_t>(n));
        return n;
    }
};

struct TestShell : ShellClient {
    char currentDirectory[128];
    int cwdCalls = 0;
    char body[128];
    int notifyCalls = 0;
    void setCurrentDirectory(std::string_view path) override
    {
        cwdCalls++;
        int n = static_cast<int>(path.size());
        if (n > static_cast<int>(sizeof(currentDirectory)) - 1)
            n = static_cast<int>(sizeof(currentDirectory)) - 1;
        std::memcpy(currentDirectory, path.data(), static_cast<size_t>(n));
        currentDirectory[n] = 0;
    }
    void postNotification(std::string_view /*title*/, std::string_view b) override
    {
        notifyCalls++;
        int bodyLength = static_cast<int>(b.size());
        if (bodyLength > static_cast<int>(sizeof(body)) - 1)
            bodyLength = static_cast<int>(sizeof(body)) - 1;
        std::memcpy(body, b.data(), static_cast<size_t>(bodyLength));
        body[bodyLength] = 0;
    }
};

static const unsigned char fileBytes[] = { 0xFF, 0x00, 0x00 }; // 1 red RGB pixel
struct TestFileRead : FileReadClient {
    int readCalls = 0;
    bool sawUnlink = false;
    [[nodiscard]] int readFile(const char* /*path*/, bool unlinkAfter, unsigned char* outputBuffer, int outputCapacity) override
    {
        readCalls++;
        sawUnlink = unlinkAfter;
        if (outputCapacity < static_cast<int>(sizeof(fileBytes)))
            return -1;
        std::memcpy(outputBuffer, fileBytes, sizeof(fileBytes));
        return static_cast<int>(sizeof(fileBytes));
    }
    [[nodiscard]] bool supportsTemporaryFileTransport() const override { return true; }
};

struct TestDiagnostics : DiagnosticsClient {
    int unknownCalls = 0;
    int lastCode = -1;
    void onUnknownOSCCode(int code, std::string_view /*payload*/) override
    {
        unknownCalls++;
        lastCode = code;
    }
};

struct TestWindow : WindowClient {
    int invalidateCalls = 0;
    void invalidate() override { invalidateCalls++; }
};

struct TestTrigger : TriggerClient { };

struct TestCapabilityPolicy : CapabilityPolicy {
    [[nodiscard]] CapabilityDecision check(Capability, CapabilityOrigin, std::string_view) override { return CapabilityDecision::Deny; }
};

// ---- tests --------------------------------------------------------

// RIS (ESC c) routes through the in-place hard-reset seam. Per-Ubiquitty::Terminal clients
// are wired once by the frontend, so every client must survive that reset.
// RIS reseeds Terminal::palette to defaults; it must also fire the repaint nudge
// (WindowClient::invalidate) so the frontend resyncs its paint cache from the
// reset palette. Without it RIS clears the screen but keeps painting the colors
// a prior OSC 4/10/11/12 left in the cache.
// ---- selection-color PAL hooks (still PAL surface) ----------------

static int g_selectionBackgroundSetCalls = 0, g_selectionBackgroundResetCalls = 0;
static uint16_t g_selectionBackgroundRed = 0, g_selectionBackgroundGreen = 0, g_selectionBackgroundBlue = 0;
static void selectionBackgroundSetCallback(Ubiquitty::Terminal*, uint16_t red, uint16_t green, uint16_t blue)
{
    g_selectionBackgroundSetCalls++;
    g_selectionBackgroundRed = red;
    g_selectionBackgroundGreen = green;
    g_selectionBackgroundBlue = blue;
}
static void selectionBackgroundResetCallback(Ubiquitty::Terminal*) { g_selectionBackgroundResetCalls++; }
static int g_selectionForegroundSetCalls = 0, g_selectionForegroundResetCalls = 0;
static void selectionForegroundSetCallback(Ubiquitty::Terminal*, uint16_t, uint16_t, uint16_t) { g_selectionForegroundSetCalls++; }
static void selectionForegroundResetCallback(Ubiquitty::Terminal*) { g_selectionForegroundResetCalls++; }

// The required clock hook every test Context must carry.
static Ubiquitty::Ticks testClock() { return Ubiquitty::Ticks(42); }

// Required-hook validation: a Context missing the required clock is invalid; one
// with it is valid. installContext/ScopedContext abort (fail-closed) on an
// invalid context, so assert the predicate rather than trigger the abort.
static bool decoderA(std::span<const std::uint8_t>, uint8_t**, int*, int*) { return true; }
static bool decoderB(std::span<const std::uint8_t>, uint8_t**, int*, int*) { return false; }

// Scoped installs nest + restore: an inner context is visible only within its
// scope, each reads its OWN hook, and the outer context returns on exit — the
// two-independent-contexts isolation the charter requires, by RAII not order.
TEST(testClipboardRoutes)
{
    TestClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    // "Hi!" → base64 "SGkh".
    feed(t, "\033]52;c;SGkh\033\\");
    CHECK(clip.setCalls == 1);
    CHECK(clip.length == 3);
    CHECK(std::memcmp(clip.data, "Hi!", 3) == 0);

    // Query routes through readClipboard ("Hi" → "SGk="). The inline read+
    // reply is the Allow policy; the default read gate is Prompt.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    char outputBuffer[64];
    feed(t, "\033]52;c;?\033\\");
    int n = Ubiquitty::drainOutput(&t, { outputBuffer, static_cast<std::size_t>(sizeof(outputBuffer) - 1) });
    outputBuffer[n] = 0;
    CHECK(std::strstr(outputBuffer, "\033]52;c;SGk=\033\\") != nullptr);
}

TEST(testShellRoutes)
{
    TestShell shell;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.shellClient = &shell;

    feed(t, "\033]7;file:///usr/local\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(std::strcmp(shell.currentDirectory, "/usr/local") == 0);

    feed(t, "\033]9;Build finished\007");
    CHECK(shell.notifyCalls == 1);
    CHECK(std::strcmp(shell.body, "Build finished") == 0);
}

TEST(testRISPreservesClients)
{
    TestWindow window;
    TestClipboard clipboard;
    TestShell shell;
    TestDiagnostics diagnostics;
    TestFileRead fileRead;
    TestTrigger trigger;
    TestCapabilityPolicy capabilityPolicy;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.windowClient = &window;
    t.clipboardClient = &clipboard;
    t.shellClient = &shell;
    t.diagnosticsClient = &diagnostics;
    t.fileReadClient = &fileRead;
    t.triggerClient = &trigger;
    t.capabilityPolicy = &capabilityPolicy;
    t.currentCapabilityOrigin = CapabilityOrigin::Agent;

    feed(t, "\033c"); // RIS

    CHECK(t.windowClient.isInstalled(&window));
    CHECK(t.clipboardClient.get() == &clipboard);
    CHECK(t.shellClient.get() == &shell);
    CHECK(t.diagnosticsClient.get() == &diagnostics);
    CHECK(t.fileReadClient.get() == &fileRead);
    CHECK(t.triggerClient.get() == &trigger);
    CHECK(t.capabilityPolicy.isInstalled(&capabilityPolicy));
    CHECK(t.currentCapabilityOrigin == CapabilityOrigin::Agent);
    // The capabilityPolicy survives the reset too, so it still gates OSC 7: this
    // test's deny-all policy correctly skips the SetWorkingDirectory write + the
    // shellClient hook — proving the policy survives and is enforced after RIS.
    feed(t, "\033]7;file:///after/ris\033\\");
    CHECK(shell.cwdCalls == 0);
    // Drop the gate and re-issue: the shellClient hook itself survived the reset and
    // fires when the write is allowed (the cwd sink is wired, not lost).
    t.capabilityPolicy = nullptr;
    feed(t, "\033]7;file:///after/ris\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(std::strcmp(shell.currentDirectory, "/after/ris") == 0);
}

TEST(testRISRepaintsPalette)
{
    TestWindow win;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.windowClient = &win;

    const Ubiquitty::PaletteRGB baseline = t.palette.indexed[1];

    // OSC 4: recolour index 1 — changes the palette and nudges invalidate.
    feed(t, "\033]4;1;rgb:ffff/0000/0000\033\\");
    CHECK(win.invalidateCalls == 1);
    CHECK(t.palette.indexed[1].red == 0xFFFF);

    // RIS resets the indexed palette to its baseline and fires the nudge again.
    feed(t, "\033c");
    CHECK(win.invalidateCalls == 2);
    CHECK(t.palette.indexed[1].red == baseline.red);
    CHECK(t.palette.indexed[1].green == baseline.green);
    CHECK(t.palette.indexed[1].blue == baseline.blue);
}

TEST(testFileReadRoutes)
{
    TestFileRead fileRead;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.fileReadClient = &fileRead;

    // base64("/tmp/tty-graphics-protocol-x") = "L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wteA=="; t=t requests unlink-after.
    feed(t, "\033_Ga=T,t=t,f=24,s=1,v=1,i=99;L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wteA==\033\\");
    CHECK(fileRead.readCalls == 1);
    CHECK(fileRead.sawUnlink == true);
}

TEST(testDiagnosticsRoutes)
{
    TestDiagnostics diag;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.diagnosticsClient = &diag;

    // An OSC code with no registered handler reports via onUnknownOSCCode.
    feed(t, "\033]64321;whatever\033\\");
    CHECK(diag.unknownCalls == 1);
    CHECK(diag.lastCode == 64321);
}

TEST(testNoClientIsSilent)
{
    /// No clients attached: OSC / APC are parsed + consumed but no-op.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]52;c;SGkh\033\\");
    feed(t, "\033]7;file:///etc\033\\");
    char outputBuffer[16];
    int n = Ubiquitty::drainOutput(&t, outputBuffer);
    CHECK(n == 0);
}

TEST(testSelectionColorPal)
{
    // No context installed → the optional selection hooks read null (degraded).
    CHECK(PAL::selectionBackgroundSetter() == nullptr);
    CHECK(PAL::selectionForegroundSetter() == nullptr);
    {
        PAL::Context ctx;
        ctx.monotonicClock = &testClock; // required
        ctx.selectionBackgroundSet = &selectionBackgroundSetCallback;
        ctx.selectionBackgroundReset = &selectionBackgroundResetCallback;
        ctx.selectionForegroundSet = &selectionForegroundSetCallback;
        ctx.selectionForegroundReset = &selectionForegroundResetCallback;
        PAL::ScopedContext scoped(ctx);
        CHECK(PAL::selectionBackgroundSetter() == &selectionBackgroundSetCallback);
        PAL::selectionBackgroundSetter()(nullptr, 0x1234, 0x5678, 0x9abc);
        CHECK(g_selectionBackgroundSetCalls == 1);
        CHECK(g_selectionBackgroundRed == 0x1234 && g_selectionBackgroundGreen == 0x5678 && g_selectionBackgroundBlue == 0x9abc);
        PAL::selectionBackgroundResetter()(nullptr);
        CHECK(g_selectionBackgroundResetCalls == 1);
        PAL::selectionForegroundSetter()(nullptr, 0, 0, 0);
        CHECK(g_selectionForegroundSetCalls == 1);
        PAL::selectionForegroundResetter()(nullptr);
        CHECK(g_selectionForegroundResetCalls == 1);
    }
    // Scope exit RESTORED the prior state (isolation by RAII, not cleanup order).
    CHECK(PAL::selectionBackgroundSetter() == nullptr);
    CHECK(PAL::selectionForegroundSetter() == nullptr);
}

TEST(testContextRequiredHooks)
{
    PAL::Context missing; // no clock
    CHECK(!missing.hasRequiredHooks());
    PAL::Context ok;
    ok.monotonicClock = &testClock;
    CHECK(ok.hasRequiredHooks());
}

TEST(testContextScopeIsolation)
{
    CHECK(PAL::imageDecoder() == nullptr); // no context
    {
        PAL::Context a;
        a.monotonicClock = &testClock;
        a.imageDecoder = &decoderA;
        PAL::ScopedContext outer(a);
        CHECK(PAL::imageDecoder() == &decoderA);
        {
            PAL::Context b;
            b.monotonicClock = &testClock;
            b.imageDecoder = &decoderB;
            PAL::ScopedContext inner(b);
            CHECK(PAL::imageDecoder() == &decoderB); // inner owner only
        }
        CHECK(PAL::imageDecoder() == &decoderA); // inner restored → outer
    }
    CHECK(PAL::imageDecoder() == nullptr); // outer restored → none
}

UBIQUITTY_TEST_MAIN()
