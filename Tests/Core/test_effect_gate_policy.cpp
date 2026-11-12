// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/EffectGate.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;
using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

/// Answers a fixed decision and records what it was asked.
class RecordingPolicy final : public CapabilityPolicy {
public:
    explicit RecordingPolicy(CapabilityDecision decision)
        : m_decision(decision)
    {
    }

    CapabilityDecision check(Capability capability, CapabilityOrigin origin, std::string_view detail) override
    {
        m_capabilities.push_back(capability);
        m_origins.push_back(origin);
        m_details.emplace_back(detail);
        return m_decision;
    }

    std::vector<Capability> m_capabilities;
    std::vector<CapabilityOrigin> m_origins;
    std::vector<std::string> m_details;

private:
    CapabilityDecision m_decision;
};

/// Counts the focus steals OSC 1337 StealFocus asks for.
class FocusRecordingWindowClient final : public WindowClient {
public:
    void stealFocus() override { m_focusSteals++; }

    int m_focusSteals { 0 };
};

/// Counts the OSC 52 writes that reach the clipboard.
class WriteCountingClipboard final : public ClipboardClient {
public:
    void writeClipboard(ClipboardBuffer, std::span<const char>) override { m_writes++; }

    int m_writes { 0 };
};

/// A host-escaping effect, a screen read and a device-attributes query, fed to
/// a terminal at `origin` with no policy installed.
struct UnsetPolicyOutcome {
    int focusSteals { 0 };
    bool didAnswerChecksum { false };
    bool didAnswerDeviceAttributes { false };
};

/// Drives the three sequences through a terminal with no policy installed.
///
/// - Parameters:
///   - origin: The principal the terminal is stamped with.
/// - Returns: What each sequence reached.
UnsetPolicyOutcome driveWithoutAPolicy(CapabilityOrigin origin)
{
    Terminal t;
    hardReset(t);
    resize(t, 5, 40);
    FocusRecordingWindowClient window;
    t.windowClient = &window;
    t.currentCapabilityOrigin = origin;
    CHECK(!t.capabilityPolicy.installed());

    UnsetPolicyOutcome outcome;
    char buffer[512];
    feed(t, "\x1b]1337;StealFocus\x07");
    outcome.focusSteals = window.m_focusSteals;
    (void)drain(t, buffer);
    feed(t, "\x1b[12;0;1;1;1;1*y"); // DECRQCRA, ScreenContentRead
    outcome.didAnswerChecksum = drain(t, buffer) > 0;
    feed(t, "\x1b[c"); // DA1, DeviceAttributesRead
    outcome.didAnswerDeviceAttributes = drain(t, buffer) > 0;

    t.windowClient = nullptr;
    return outcome;
}

/// Feeds one OSC 52 write to a terminal with no policy installed.
///
/// - Parameters:
///   - origin: The principal the terminal is stamped with.
/// - Returns: How many writes reached the clipboard.
int clipboardWritesWithoutAPolicy(CapabilityOrigin origin)
{
    Terminal t;
    hardReset(t);
    WriteCountingClipboard clipboard;
    t.clipboardClient = &clipboard;
    t.currentCapabilityOrigin = origin;
    CHECK(!t.capabilityPolicy.installed());
    feed(t, "\x1b]52;c;SGk=\x1b\\");
    t.clipboardClient = nullptr;
    return clipboard.m_writes;
}

}

/// With no policy installed the two gates part company, which is the whole
/// reason there are two of them.
TEST(testAnUnsetPolicyAllowsButDoesNotGrant)
{
    Terminal t;
    hardReset(t);
    CHECK(!t.capabilityPolicy.installed());
    CHECK(effectAllowed(t, Capability::ClipboardWrite));
    CHECK(!effectGranted(t, Capability::ClipboardWrite));
}

/// A Local terminal with no policy is allowed every capability, as every
/// frontend's policy allows Local, so leaving one out changes nothing for a
/// program's own output.
TEST(testAnUnsetPolicyAllowsEveryLocalEffect)
{
    Terminal t;
    hardReset(t);
    CHECK(t.currentCapabilityOrigin == CapabilityOrigin::Local);
    int firstRefused = -1;
    for (int raw = static_cast<int>(Capability::None); raw >= 0; --raw) {
        if (!effectAllowed(t, static_cast<Capability>(raw)))
            firstRefused = raw;
    }
    CHECK_EQ(firstRefused, -1);

    const UnsetPolicyOutcome outcome = driveWithoutAPolicy(CapabilityOrigin::Local);
    CHECK_EQ(outcome.focusSteals, 1);
    CHECK(outcome.didAnswerChecksum);
    CHECK(outcome.didAnswerDeviceAttributes);
    CHECK_EQ(clipboardWritesWithoutAPolicy(CapabilityOrigin::Local), 1);
}

/// A Remote terminal with no policy gets PAL's Remote defaults rather than
/// everything: a host escape and a screen read are refused, and a
/// device-attributes query still answers, so a program waiting on it does not
/// hang. A tmux pane stamped Remote parses its output before any view installs
/// a policy, and this is the answer it gets meanwhile.
TEST(testAnUnsetPolicyRefusesARemoteHostEscapeAndExfil)
{
    Terminal t;
    hardReset(t);
    t.currentCapabilityOrigin = CapabilityOrigin::Remote;
    CHECK(!effectAllowed(t, Capability::StealFocus)); // HostEscaping
    CHECK(!effectAllowed(t, Capability::WindowMutate)); // HostEscaping
    CHECK(!effectAllowed(t, Capability::ScreenContentRead)); // ReadExfil
    CHECK(!effectAllowed(t, Capability::ClipboardRead)); // ReadExfil
    CHECK(effectAllowed(t, Capability::DeviceAttributesRead)); // Benign
    CHECK(effectAllowed(t, Capability::SetHyperlink)); // LowRiskInfo
    CHECK(effectAllowed(t, Capability::ImageRender)); // Rendering

    const UnsetPolicyOutcome outcome = driveWithoutAPolicy(CapabilityOrigin::Remote);
    CHECK_EQ(outcome.focusSteals, 0);
    CHECK(!outcome.didAnswerChecksum);
    CHECK(outcome.didAnswerDeviceAttributes);
}

/// An Agent terminal with no policy is refused what PAL's Agent default would
/// prompt for: with nothing installed to ask, a Prompt is a refusal.
TEST(testAnUnsetPolicyRefusesAnAgentHostEscapeAndExfil)
{
    Terminal t;
    hardReset(t);
    t.currentCapabilityOrigin = CapabilityOrigin::Agent;
    CHECK(!effectAllowed(t, Capability::StealFocus));
    CHECK(!effectAllowed(t, Capability::ScreenContentRead));
    CHECK(effectAllowed(t, Capability::DeviceAttributesRead));

    const UnsetPolicyOutcome outcome = driveWithoutAPolicy(CapabilityOrigin::Agent);
    CHECK_EQ(outcome.focusSteals, 0);
    CHECK(!outcome.didAnswerChecksum);
    CHECK(outcome.didAnswerDeviceAttributes);
}

/// PAL's default asks a remote owner about a clipboard write, and with no
/// policy there is nothing to hold the payload for consent. The write is
/// refused rather than delivered, and the Prompt does not reach for a policy
/// that is not there.
TEST(testAnUnsetPolicyRefusesARemoteClipboardWrite)
{
    CHECK(defaultDecision(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK_EQ(clipboardWritesWithoutAPolicy(CapabilityOrigin::Remote), 0);
    CHECK_EQ(clipboardWritesWithoutAPolicy(CapabilityOrigin::Agent), 0);
}

/// Prompt is a denial for this pass on both gates: Core cannot block for an
/// answer, so a parked request is refused now and re-driven after a grant.
TEST(testPromptRefusesOnBothGates)
{
    Terminal t;
    hardReset(t);
    RecordingPolicy prompt(CapabilityDecision::Prompt);
    t.capabilityPolicy = &prompt;

    CHECK(!effectAllowed(t, Capability::ClipboardWrite));
    CHECK(!effectGranted(t, Capability::ClipboardWrite));

    RecordingPolicy deny(CapabilityDecision::Deny);
    t.capabilityPolicy = &deny;
    CHECK(!effectAllowed(t, Capability::ClipboardWrite));
    CHECK(!effectGranted(t, Capability::ClipboardWrite));

    RecordingPolicy allow(CapabilityDecision::Allow);
    t.capabilityPolicy = &allow;
    CHECK(effectAllowed(t, Capability::ClipboardWrite));
    CHECK(effectGranted(t, Capability::ClipboardWrite));

    t.capabilityPolicy = nullptr; // the terminal outlives these fixtures
}

/// Both gates hand the policy the origin and the detail, so a policy that keys
/// on either sees what the caller was given.
TEST(testTheGatesForwardOriginAndDetail)
{
    Terminal t;
    hardReset(t);
    RecordingPolicy policy(CapabilityDecision::Allow);
    t.capabilityPolicy = &policy;
    t.currentCapabilityOrigin = CapabilityOrigin::Remote;

    CHECK(effectAllowed(t, Capability::FileReadAccess, "/tmp/one"));
    CHECK(effectGranted(t, Capability::FileReadAccess, "/tmp/two"));

    CHECK_EQ(static_cast<int>(policy.m_capabilities.size()), 2);
    CHECK(policy.m_capabilities[0] == Capability::FileReadAccess);
    CHECK(policy.m_origins[0] == CapabilityOrigin::Remote);
    CHECK(policy.m_origins[1] == CapabilityOrigin::Remote);
    // The payload a policy keys on: dropping it would gate on less than the
    // caller was handed, and every caller would still compile.
    CHECK(policy.m_details[0] == "/tmp/one");
    CHECK(policy.m_details[1] == "/tmp/two");

    t.capabilityPolicy = nullptr;
}

/// A reset preserves the installed policy without anything holding its pointer.
TEST(testAResetPreservesTheInstalledPolicy)
{
    Terminal t;
    hardReset(t);
    RecordingPolicy policy(CapabilityDecision::Deny);
    t.capabilityPolicy = &policy;
    CHECK(t.capabilityPolicy.isInstalled(&policy));

    hardReset(t);
    CHECK(t.capabilityPolicy.isInstalled(&policy));
    CHECK(!effectAllowed(t, Capability::SetWindowTitle)); // still gated after RIS

    t.capabilityPolicy = nullptr;
}

TEST(testRestrictivePolicyKeepsProtocolRepliesAndGridMetadata)
{
    RestrictiveCapabilityPolicy policy;
    for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 5, 40);
        terminal.capabilityPolicy = &policy;
        terminal.currentCapabilityOrigin = origin;

        feed(terminal, "\033[5n\033[2;3H\033[6n\033[18t\033[?1$p");
        char outputBuffer[256];
        (void)drain(terminal, outputBuffer);
        CHECK_STR_EQ(std::string(outputBuffer), "\033[0n\033[2;3R\033[8;5;40t\033[?1;2$y");

        feed(terminal, "\033[61\"p\033[c");
        (void)drain(terminal, outputBuffer);
        CHECK_STR_EQ(std::string(outputBuffer), "\033[?1;2c");

        feed(terminal, "\033]2;embedded-terminal\007\033[2;3Htext");
        CHECK_STR_EQ(terminal.titleUTF8, "embedded-terminal");
        const Cell* first = UbiquittyTest::cell(terminal, 1, 2);
        REQUIRE(first);
        CHECK_EQ(first->codePoint, static_cast<std::uint32_t>('t'));
    }
}

TEST(testRestrictivePolicyStopsExternalEffectsAtTheirSinks)
{
    RestrictiveCapabilityPolicy policy;
    FocusRecordingWindowClient window;
    WriteCountingClipboard clipboard;
    for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 5, 40);
        terminal.capabilityPolicy = &policy;
        terminal.currentCapabilityOrigin = origin;
        terminal.windowClient = &window;
        terminal.clipboardClient = &clipboard;

        feed(terminal, "\033]1337;StealFocus\007\033]52;c;SGk=\033\\\033[7;8;25,~");
        CHECK_EQ(window.m_focusSteals, 0);
        CHECK_EQ(clipboard.m_writes, 0);
        CHECK(!terminal.pendingSound || !terminal.pendingSound->count);

        feed(terminal, "\033[12;0;1;1;1;1*y\033[20t\033[21t");
        char outputBuffer[256];
        CHECK_EQ(drain(terminal, outputBuffer), 0);
        CHECK(!effectGranted(terminal, Capability::FileReadAccess, "/tmp/embedded-image"));
    }
}

TEST(testRestrictivePolicyRefusesUnknownCapabilitiesAndOrigins)
{
    RestrictiveCapabilityPolicy policy;
    Terminal terminal;
    hardReset(terminal);
    terminal.capabilityPolicy = &policy;

    CHECK(!effectAllowed(terminal, static_cast<Capability>(0xffff)));
    CHECK(!effectGranted(terminal, static_cast<Capability>(0xffff)));
    terminal.currentCapabilityOrigin = static_cast<CapabilityOrigin>(0xff);
    CHECK(!effectAllowed(terminal, Capability::StealFocus));
    CHECK(!effectAllowed(terminal, Capability::DeviceAttributesRead));
    CHECK(!effectAllowed(terminal, Capability::ImageRender));
    CHECK(!effectAllowed(terminal, Capability::SetWindowTitle));
}

TEST(testCustomDecisionsControlActualClipboardAndWindowEffects)
{
    for (const CapabilityDecision decision : { CapabilityDecision::Allow, CapabilityDecision::Deny, CapabilityDecision::Prompt }) {
        RecordingPolicy policy(decision);
        FocusRecordingWindowClient window;
        WriteCountingClipboard clipboard;
        Terminal terminal;
        hardReset(terminal);
        terminal.capabilityPolicy = &policy;
        terminal.currentCapabilityOrigin = CapabilityOrigin::Agent;
        terminal.windowClient = &window;
        terminal.clipboardClient = &clipboard;

        feed(terminal, "\033]1337;StealFocus\007\033]52;c;SGk=\033\\");
        const int expectedEffects = decision == CapabilityDecision::Allow ? 1 : 0;
        CHECK_EQ(window.m_focusSteals, expectedEffects);
        CHECK_EQ(clipboard.m_writes, expectedEffects);
        REQUIRE(!policy.m_origins.empty());
        for (const CapabilityOrigin origin : policy.m_origins)
            CHECK(origin == CapabilityOrigin::Agent);
    }
}

UBIQUITTY_TEST_MAIN()
