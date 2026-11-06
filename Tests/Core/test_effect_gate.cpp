// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

// test_effect_gate — the write side's chokepoint, and the property that makes
// it one.
//
// A reply leaves Core only through respond(), which takes a Capability. An
// effect reaches a frontend only through a GatedClient, whose pointer is
// private and whose only readers take a Capability, so a site that names none
// does not compile. The first case asserts exactly that, in the only form a
// test can: a concept that is satisfied when the escape exists.
//
// The rest drive the real HostCapabilityPolicy tier table through sequences,
// because a gate that compiles and never denies would pass the first case.

#include "Core/EffectGate.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

using UbiquittyTest::feed;

namespace {

// Satisfied when a raw pointer can be taken out of the gate without naming a
// capability. It covers every spelling that would reopen the hole: a CheckedPtr
// accessor, an implicit conversion, a dereference, an arrow.
template<typename Gate>
concept YieldsClientWithoutCapability = requires(Gate gate) { gate.get(); }
    || requires(Gate gate) { *gate; }
    || requires(Gate gate) { gate.operator->(); }
    || requires(Gate gate) { static_cast<Ubiquitty::WindowClient*>(gate); };

// A window client that records the one thing each case asks about.
class RecordingWindowClient final : public Ubiquitty::WindowClient {
public:
    void stealFocus() override { focusSteals++; }
    [[nodiscard]] bool setFont(std::string_view family, double, double) override
    {
        fontSets++;
        lastFamily = std::string(family);
        return true;
    }
    void invalidate() override { invalidations++; }

    int focusSteals { 0 };
    int fontSets { 0 };
    int invalidations { 0 };
    std::string lastFamily;
};

// The informational tier is Allow for Remote by default; the font cases are
// about a refused set, so they run under a host that restricts it to Prompt.
constexpr Ubiquitty::CapabilityDecisionTable promptingRemote = Ubiquitty::CapabilityDecisionTable {}.replacing(
    Ubiquitty::CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::LowRiskInfo, Ubiquitty::CapabilityDecision::Prompt);

// The production policy at a chosen origin, with a recording client installed.
template<typename Body> void withTerminal(Ubiquitty::CapabilityOrigin origin, const Ubiquitty::CapabilityDecisionTable& decisions, Body&& body)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    RecordingWindowClient window;
    Ubiquitty::HostCapabilityPolicy policy(decisions);
    t.windowClient = &window;
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = origin;
    body(t, window);
    t.capabilityPolicy = nullptr;
    t.windowClient = nullptr;
}

template<typename Body> void withTerminal(Ubiquitty::CapabilityOrigin origin, Body&& body)
{
    withTerminal(origin, Ubiquitty::CapabilityDecisionTable {}, std::forward<Body>(body));
}

} // namespace

// The structural property. Not "no site currently forgets" — that is a fact
// about today's tree that a future commit changes silently. This asserts that
// no site is able to forget, and it fails the moment someone adds an accessor
// that hands the client out unconditionally.
//
// Fails without the gate: with a public CheckedPtr field, `gate.get()` is
// well-formed and the concept is satisfied.
TEST(testTheGateYieldsNoClientWithoutNamingACapability)
{
    static_assert(!YieldsClientWithoutCapability<Ubiquitty::GatedClient<Ubiquitty::WindowClient>>,
        "a client can be reached without naming a capability: the effect chokepoint is decorative");
    CHECK(!YieldsClientWithoutCapability<Ubiquitty::GatedClient<Ubiquitty::WindowClient>>);
}

// OSC 1337 StealFocus is HostEscaping, so Remote is denied and Local allowed.
// The pair is the point: a gate that denied everything would pass the first
// half alone.
TEST(testAHostEscapingEffectIsDeniedAtRemoteAndAllowedAtLocal)
{
    withTerminal(Ubiquitty::CapabilityOrigin::Remote, [](Ubiquitty::Terminal& t, RecordingWindowClient& window) {
        feed(t, "\x1b]1337;StealFocus\x07");
        CHECK(window.focusSteals == 0);
    });
    withTerminal(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t, RecordingWindowClient& window) {
        feed(t, "\x1b]1337;StealFocus\x07");
        CHECK(window.focusSteals == 1);
    });
}

// OSC 50 reached setFont with no check at all, while DECDLD asked about the
// same Capability::SetFont. SetFont is LowRiskInfo; with that tier restricted
// to Prompt for Remote — which Core treats as deny-for-now — the Remote set is
// refused, and Local applies.
//
// Fails without the gate: the Remote case records a font set, because the OSC
// 50 path called the client directly.
TEST(testTheFontSetAsksTheSameCapabilityOnBothOfItsRoutes)
{
    withTerminal(Ubiquitty::CapabilityOrigin::Remote, promptingRemote, [](Ubiquitty::Terminal& t, RecordingWindowClient& window) {
        feed(t, "\x1b]50;Menlo\x07");
        CHECK(window.fontSets == 0);
    });
    withTerminal(Ubiquitty::CapabilityOrigin::Local, [](Ubiquitty::Terminal& t, RecordingWindowClient& window) {
        feed(t, "\x1b]50;Menlo\x07");
        CHECK(window.fontSets == 1);
        CHECK(window.lastFamily == "Menlo");
    });
}

TEST(aRefusedFontRequestPreservesThePublishedQueryFamily)
{
    withTerminal(Ubiquitty::CapabilityOrigin::Remote, promptingRemote, [](Ubiquitty::Terminal& terminal, RecordingWindowClient& window) {
        Ubiquitty::setFontString(terminal, "Accepted Face");
        feed(terminal, "\x1b]50;Denied Face:size=17\x07");
        CHECK_EQ(window.fontSets, 0);
        CHECK(terminal.font == "Accepted Face");
        CHECK_EQ(terminal.fontRequests.generation(), 0u);
        CHECK(!terminal.fontRequestChanged);
        feed(terminal, "\x1b]50;?\x07");
        CHECK(std::string(terminal.outputBuffer.data(), static_cast<std::size_t>(terminal.outputLength))
            == "\x1b]50;Accepted Face\x07");
    });
}

// The escape hatch reaches the client whatever the policy says, which is the
// whole reason it exists: a repaint reveals nothing, and paying a virtual
// policy call per frame to be told so would be the cost without the benefit.
//
// Driven directly rather than through a sequence. The obvious sequence — an
// OSC 4 palette set, which repaints — is gated upstream at Remote by SetColor,
// so it exercises that gate and says nothing about this one. A case whose
// subject is decided before it is reached is a case about something else.
//
// Fails without the hatch: route invalidate() through effectClient and a
// deny-all policy stops the repaint.
TEST(testTheEscapeHatchIgnoresThePolicyByDesign)
{
    class DenyEverything final : public Ubiquitty::CapabilityPolicy {
    public:
        Ubiquitty::CapabilityDecision check(Ubiquitty::Capability, Ubiquitty::CapabilityOrigin, std::string_view) override
        {
            return Ubiquitty::CapabilityDecision::Deny;
        }
    };

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    RecordingWindowClient window;
    DenyEverything policy;
    t.windowClient = &window;
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    CHECK(Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient) == &window);
    // And the gated accessor refuses the same client under the same policy, so
    // the pair shows the hatch is a different door rather than the same one.
    CHECK(Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::StealFocus) == nullptr);

    t.capabilityPolicy = nullptr;
    t.windowClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
