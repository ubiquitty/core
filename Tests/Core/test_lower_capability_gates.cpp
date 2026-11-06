// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

// test_lower_capability_gates — conformance tests for lower-severity capability
// gates, driven through the REAL HostCapabilityPolicy tier table.
//
// === GATED (all replies flow through respond, which calls check()) ===
//
//   Answerback (HostEscaping → Remote=Deny, Local=Allow):
//     ENQ 0x05 → answerback string.  Remote: suppressed.  Local: emitted.
//     verify-by-revert: re-tier Answerback to Benign and the Remote CHECK(false)
//     flips — the reply reappears.
//
//   DA2 (Benign → Remote=Allow):
//     CSI > c → ESC[>41;…;0c.  Emitted at both Remote and Local.
//     verify-by-revert: mis-tier DeviceAttributesRead to ReadExfil and the
//     Remote CHECK(true) flips — the reply is suppressed, breaking
//     neovim/tmux secondary-DA probes on a teleport session.
//
//   ColorRead (Benign → Remote=Allow):
//     OSC 10;? → default-foreground report.  Emitted at both origins.
//     verify-by-revert: mis-tier ColorRead to LowRiskInfo (Remote=Prompt) and
//     the Remote CHECK(true) flips — the query goes dark.
//
// === GATED WRITES (LowRiskInfo → Remote=Prompt when restricted: suppressed, Agent/Local=Allow) ===
//
// LowRiskInfo is Allow for Remote by default; these cases are about the gates,
// so every policy here is the real table with that tier restricted to Prompt —
// the configuration of a host that distrusts remote presentation.
//
// These three LowRiskInfo capabilities gate their SET paths through
// policy->check(); a Remote-origin write is suppressed (no grant), while the
// same write at Agent/Local origin applies — proving the gate is origin-scoped,
// not a blanket block:
//
//   SetHyperlink (OSC 8 → currentLinkID):  handleHyperlink (State.cpp).
//   SetColor-SET (OSC 4/5/10/11/12 → palette): handleIndexedColor, handleSpecialColor,
//     applyDefaultColorField, handleCursorColor (Color.cpp) — the fg=bg
//     content-hiding vector.
//   SetCursorShape (DECSCUSR CSI <n> SP q; OSC 1337 CursorShape=): csiH_decscusr
//     (VT/Prefixed.cpp) + ITerm2.cpp.
//
// verify-by-revert: delete the `check() != Allow → return/continue` gate at any
// chokepoint and its *_suppressed_at_remote assertion flips (the write reappears
// at Remote).  The *_applied_at_agent contrast guards against over-blocking.
//
// SetWindowTitle (OSC 2 → titleUTF8) is Benign → Remote=Allow and has no
// check(); that is correct — a pure display-string write reveals nothing.  The
// test confirms it passes at both origins as intended.

#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>

// Feed a single raw byte (ENQ, BEL, etc.).
static void feedByte(Ubiquitty::Terminal& t, uint8_t b)
{
    const char c = static_cast<char>(b);
    Ubiquitty::processBytes(&t, std::string_view(&c, static_cast<std::size_t>(1)));
}

// Drain the output queue into out (NUL-terminated); returns byte count.

// The real tier table with Remote's informational tier restricted to Prompt.
static constexpr Ubiquitty::CapabilityDecisionTable promptingRemote = Ubiquitty::CapabilityDecisionTable {}.replacing(
    Ubiquitty::CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::LowRiskInfo, Ubiquitty::CapabilityDecision::Prompt);

// Install the production policy at a given origin and run body(&t).
// Mirrors the pattern in test_high2_write_gate / test_high2_readexfil_gate.
template<typename Body> static void withTerm(Ubiquitty::CapabilityOrigin origin, Body&& body)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    Ubiquitty::HostCapabilityPolicy policy(promptingRemote);
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = origin;
    body(t);
    t.capabilityPolicy = nullptr;
}

// ---------------------------------------------------------------------------
// Answerback — ENQ (0x05) → answerback string (HostEscaping → Remote=Deny).
//
// actExecuteAnswerback calls respond(t, Capability::Answerback, …) which
// is the single check() chokepoint.  HostEscaping→Remote=Deny so at Remote
// the reply must not appear.  At Local it does.
//
// verify-by-revert: change HostCapabilityPolicy::tierOf(Answerback) to return
// Benign and CHECK(!…) at Remote flips — the reply reappears.
// ---------------------------------------------------------------------------
static const char* answerbackString = "UBIQUITTY-TERM";

// ---------------------------------------------------------------------------
// User-variable writes, attention and focus changes escape the pane. Progress
// inside the pane is presentation metadata; it grants no permission to update
// an external Dock/taskbar indicator or perform another window effect.
// ---------------------------------------------------------------------------
namespace {

struct AttentionCountingClient : Ubiquitty::WindowClient {
    int attentionRequests = 0;
    int focusSteals = 0;
    void requestAttention(Ubiquitty::AttentionRequest) override { ++attentionRequests; }
    void stealFocus() override { ++focusSteals; }
};

} // namespace

TEST(theWindowEscapesAreDeniedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        AttentionCountingClient client;
        t.windowClient = &client;
        feed(t, "\x1b]1337;RequestAttention=yes\x07");
        feed(t, "\x1b]1337;StealFocus\x07");
        CHECK_EQ(client.attentionRequests, 0);
        CHECK_EQ(client.focusSteals, 0);
        t.windowClient = nullptr;
    });
}

TEST(theWindowEscapesApplyAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        AttentionCountingClient client;
        t.windowClient = &client;
        feed(t, "\x1b]1337;RequestAttention=yes\x07");
        feed(t, "\x1b]1337;StealFocus\x07");
        CHECK_EQ(client.attentionRequests, 1);
        CHECK_EQ(client.focusSteals, 1);
        t.windowClient = nullptr;
    });
}

TEST(remoteProgressMetadataIsAllowedWhileUserVariablesAndExternalEffectsAreDenied)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        AttentionCountingClient client;
        t.windowClient = &client;
        feed(t, "\x1b]1337;SetUserVar=who=YWJj\x07"); // "abc"
        feed(t, "\x1b]9;4;1;42\x07");
        CHECK(!t.userVariables[0].used);
        // The external-progress capability remains denied; the bounded model
        // state used by pane and tab presentation is independent of it.
        CHECK(!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ProgressIndicator));
        CHECK_EQ(static_cast<int>(t.progressState), 1);
        CHECK_EQ(static_cast<int>(t.progressValue), 42);
        feed(t, "\x1b]1337;RequestAttention=yes\x07\x1b]1337;StealFocus\x07");
        CHECK_EQ(client.attentionRequests, 0);
        CHECK_EQ(client.focusSteals, 0);
        t.windowClient = nullptr;
    });
}

TEST(theUserVariableAndProgressApplyAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, "\x1b]1337;SetUserVar=who=YWJj\x07");
        feed(t, "\x1b]9;4;1;42\x07");
        CHECK(t.userVariables[0].used);
        CHECK_EQ(static_cast<int>(t.progressValue), 42);
    });
}

TEST(testAnswerbackSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        Ubiquitty::setAnswerback(&t, answerbackString);
        feedByte(t, 0x05); // ENQ
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n == 0); // HostEscaping→Remote=Deny: no reply on ENQ
    });
}

// CONTRAST: the same ENQ at origin=Local is replied (Local=Allow for any tier).
TEST(testAnswerbackEmittedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        Ubiquitty::setAnswerback(&t, answerbackString);
        feedByte(t, 0x05); // ENQ
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n > 0); // Local=Allow: answerback replied
        CHECK(std::strstr(outputBuffer, answerbackString) != nullptr);
    });
}

// ---------------------------------------------------------------------------
// DA2 — CSI > c (Secondary Device AttributeSet, Benign → Remote=Allow).
//
// csiH_da2 calls respond(Capability::DeviceAttributesRead, …) which is
// the single check() chokepoint.  DeviceAttributesRead is Benign → the reply
// must be EMITTED even at Remote (no over-block: neovim/tmux/fzf probe DA2
// to unlock capability flags; suppressing it on a teleport-viewed session
// would hang those apps mid-startup).
//
// verify-by-revert: mis-tier DeviceAttributesRead to ReadExfil and the
// Remote CHECK(n > 0) flips — the probe goes dark.
// ---------------------------------------------------------------------------
static const char* da2Query = "\033[>c"; // CSI > c

TEST(testDa2EmittedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        feed(t, da2Query);
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n > 0); // Benign→Remote=Allow: DA2 reply emitted, no over-block
    });
}

TEST(testDa2EmittedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        feed(t, da2Query);
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n > 0); // Local=Allow
    });
}

// ---------------------------------------------------------------------------
// ColorRead — OSC 10 ; ? ST (default-foreground query, Benign → Remote=Allow).
//
// applyDefaultColorField calls respond(Capability::ColorRead, …).
// ColorRead is Benign → the reply must be EMITTED at Remote (apps query this
// to detect dark/light themes; suppressing it on a shared-view session would
// produce the wrong theme).
//
// allowColorOperations must be true (it mirrors the host-level allow list, not
// the capability gate): set it explicitly since the test Ubiquitty::Terminal is zero-init.
//
// verify-by-revert: mis-tier ColorRead to LowRiskInfo (→Remote=Prompt) and
// the Remote CHECK(n > 0) flips — the query returns nothing.
// ---------------------------------------------------------------------------
static const char* osc10Query = "\033]10;?\033\\"; // OSC 10 ; ? ST

TEST(testColorreadEmittedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true; // host-level allow, not the capability gate
        feed(t, osc10Query);
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n > 0); // Benign→Remote=Allow: color report emitted
    });
}

TEST(testColorreadEmittedAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        feed(t, osc10Query);
        char outputBuffer[128];
        int n = drain(t, outputBuffer);
        CHECK(n > 0); // Local=Allow
    });
}

// ---------------------------------------------------------------------------
// SetWindowTitle — OSC 2 ; <text> ST (Benign → Remote=Allow, no check()).
//
// oscH_title copies the string via setTitleUTF8String. SetWindowTitle is
// Benign, so a Remote program setting the title is allowed — the
// title is visible to the viewer, not a hidden side-channel. No check() call
// is present or needed. The test confirms the write is not gated at Remote
// (which would be wrong for Benign), and that the side effect is observable.
//
// allowTitleOperations must be true (zero-init Ubiquitty::Terminal has it false).
// ---------------------------------------------------------------------------
static const char* setTitleSequence = "\033]2;ubiquitty-test-title\033\\";

TEST(testSetWindowTitleAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        t.allowTitleOperations = true;
        feed(t, setTitleSequence);
        // Benign → Remote=Allow: title write passes; no check() gate, correct
        CHECK(t.titleUTF8 == "ubiquitty-test-title");
    });
}

TEST(testSetWindowTitleAtLocal)
{
    withTerm(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t) {
        t.allowTitleOperations = true;
        feed(t, setTitleSequence);
        CHECK(t.titleUTF8 == "ubiquitty-test-title");
    });
}

// ---------------------------------------------------------------------------
// GATED WRITES — SetHyperlink, SetColor-SET, SetCursorShape.
//
// All three are LowRiskInfo → Remote=Prompt (≠Allow): a Remote-origin write is
// suppressed at the Core chokepoint (check() != Allow → skip), while the same
// write at Agent origin (LowRiskInfo→Agent=Allow) applies.  The Remote/Agent
// pair proves the gate is origin-scoped, not a blanket block.
// ---------------------------------------------------------------------------

// SetHyperlink: OSC 8 sets currentLinkID — gated in handleHyperlink.
static const char* osc8Open = "\033]8;;https://evil.example\033\\"; // open link

TEST(testSethyperlinkSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        uint16_t before = t.currentLinkID;
        feed(t, osc8Open);
        CHECK(t.currentLinkID == before); // Remote=Prompt: link write suppressed
    });
}

TEST(testSethyperlinkAppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        uint16_t before = t.currentLinkID;
        feed(t, osc8Open);
        CHECK(t.currentLinkID != before); // LowRiskInfo→Agent=Allow: link armed
    });
}

// SetColor (SET): OSC 4 ; 1 ; #ff0000 mutates palette.indexed — gated in handleIndexedColor.
static const char* osc4SetRed = "\033]4;1;#ff0000\033\\"; // set color-index 1 to red

TEST(testSetcolorSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.indexed[1].red;
        feed(t, osc4SetRed);
        CHECK(t.palette.indexed[1].red == rBefore); // Remote=Prompt: palette write suppressed
    });
}

TEST(testSetcolorAppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.indexed[1].red;
        feed(t, osc4SetRed);
        CHECK(t.palette.indexed[1].red != rBefore); // Agent=Allow: palette mutated
    });
}

// SetColor default-background (OSC 11): the fg=bg content-hiding vector —
// gated in applyDefaultColorField. Observable via palette.defaultBackground.
static const char* osc11SetBackground = "\033]11;#123456\033\\";

TEST(testSetcolorDefaultbgSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.defaultBackground.red;
        feed(t, osc11SetBackground);
        CHECK(t.palette.defaultBackground.red == rBefore); // suppressed — no fg=bg hide
    });
}

TEST(testSetcolorDefaultbgAppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.defaultBackground.red;
        feed(t, osc11SetBackground);
        CHECK(t.palette.defaultBackground.red != rBefore); // Agent=Allow
    });
}

// SetColor on a chained field: the gate is decided per field, so a query at the
// head of an OSC 10 chain still answers while the background set behind it is
// suppressed at Remote, and both land at Agent.
static const char* osc10QueryThenSetBackground = "\033]10;?;#123456\033\\";

TEST(testSetcolorChainedBackgroundSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.defaultBackground.red;
        feed(t, osc10QueryThenSetBackground);
        char outputBuffer[128];
        CHECK(drain(t, outputBuffer) > 0); // ColorRead is Benign: the query answers
        CHECK(t.palette.defaultBackground.red == rBefore); // the chained set is suppressed
    });
}

TEST(testSetcolorChainedBackgroundAppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        t.allowColorOperations = true;
        uint16_t rBefore = t.palette.defaultBackground.red;
        feed(t, osc10QueryThenSetBackground);
        char outputBuffer[128];
        CHECK(drain(t, outputBuffer) > 0);
        CHECK(t.palette.defaultBackground.red != rBefore);
    });
}

// SetCursorShape: DECSCUSR CSI 6 SP q (underline-steady) — gated in csiH_decscusr.
static const char* decscusr6 = "\033[6 q"; // CSI 6 SP q → underline steady

TEST(testSetcursorshapeSuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        Ubiquitty::Cursor::Style before = t.cursorStyle; // 0 on a fresh term
        feed(t, decscusr6);
        CHECK(t.cursorStyle == before); // Remote=Prompt: cursor-shape write suppressed
    });
}

TEST(testSetcursorshapeAppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        Ubiquitty::Cursor::Style before = t.cursorStyle;
        feed(t, decscusr6);
        CHECK(t.cursorStyle != before); // Agent=Allow: cursorStyle set
    });
}

// SetCursorShape via OSC 1337 CursorShape= (the iTerm2 alias path in ITerm2.cpp).
static const char* osc1337CursorShape = "\033]1337;CursorShape=1\033\\"; // vertical bar

TEST(testSetCursorShapeOSC1337SuppressedAtRemote)
{
    withTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t) {
        Ubiquitty::Cursor::Style before = t.cursorStyle;
        feed(t, osc1337CursorShape);
        CHECK(t.cursorStyle == before); // Remote=Prompt: suppressed on the alias path too
    });
}

TEST(testSetCursorShapeOSC1337AppliedAtAgent)
{
    withTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t) {
        Ubiquitty::Cursor::Style before = t.cursorStyle;
        feed(t, osc1337CursorShape);
        CHECK(t.cursorStyle != before); // Agent=Allow
    });
}

// ---------------------------------------------------------------------------
// SelectColorPreset — OSC 1337 SetColors=preset=<name> → WindowClient::
// selectColorPreset (live theme switch). LowRiskInfo → Remote=Prompt: a remote
// peer switching the theme on a shared view is display-spoofing (limited to
// named host-registered presets, so weaker than the now-gated OSC 4/10/11).
// The effect lands on a windowClient sink, so the gate is observed through a
// recording mock rather than a Ubiquitty::Terminal field. allowColorOperations must be true
// (the host-level flag is a separate upstream gate).
// ---------------------------------------------------------------------------
namespace {
struct PresetRecorder : Ubiquitty::WindowClient {
    int calls = 0;
    void selectColorPreset(std::string_view) override { calls++; }
};
}
static const char* osc1337SetPreset = "\033]1337;SetColors=preset=Nord\033\\";

// Install policy@origin + a preset recorder, run body(&t, recorder).
template<typename Body> static void withPresetTerm(Ubiquitty::CapabilityOrigin origin, Body&& body)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    Ubiquitty::HostCapabilityPolicy policy(promptingRemote);
    PresetRecorder recorder;
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = origin;
    t.windowClient = &recorder;
    t.allowColorOperations = true;
    body(t, recorder);
    t.windowClient = nullptr;
    t.capabilityPolicy = nullptr;
}

TEST(testSelectcolorpresetSuppressedAtRemote)
{
    withPresetTerm(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t, PresetRecorder& rec) {
        feed(t, osc1337SetPreset);
        CHECK(rec.calls == 0); // Remote=Prompt: preset switch suppressed
    });
}

TEST(testSelectcolorpresetAppliedAtAgent)
{
    withPresetTerm(Ubiquitty::CapabilityOrigin::Agent, [](Ubiquitty::Terminal& t, PresetRecorder& rec) {
        feed(t, osc1337SetPreset);
        CHECK(rec.calls == 1); // LowRiskInfo→Agent=Allow: preset applied
    });
}

UBIQUITTY_TEST_MAIN()
