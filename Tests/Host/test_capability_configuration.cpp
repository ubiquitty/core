// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_capability_configuration — the host policy decides by the table the
// configuration resolves to. A remote principal is the owner's own device, so a
// program it drives may set a hyperlink or a cursor shape without a prompt; the
// owner restricts that with `[capabilities.remote] informational`, and nothing in
// the file reaches the host-escaping or exfil defaults it did not name, or the
// tiers that have no key.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

using Ubiquitty::Capability;
using Ubiquitty::CapabilityConsentQueue;
using Ubiquitty::CapabilityDecision;
using Ubiquitty::CapabilityDecisionTable;
using Ubiquitty::CapabilityGrantScope;
using Ubiquitty::CapabilityGrantStore;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::CapabilityTier;
using Ubiquitty::defaultDecision;
using Ubiquitty::HostCapabilityPolicy;
using Ubiquitty::tierOf;
namespace Preferences = Ubiquitty::Preferences;

namespace {

constexpr const char* remoteIdentity = "token:owner-phone";

/// The table the daemon would build from `text`, parsed strictly so a misspelt
/// key in a case fails the case rather than silently testing the default.
CapabilityDecisionTable decisionsFrom(const std::string& text)
{
    Preferences::Values values;
    std::istringstream input(text);
    CHECK_MSG(Preferences::parse(input, values, Preferences::ParseMode::Strict), text.c_str());
    return Preferences::Effective::capabilityDecisions(values);
}

/// A session policy over `decisions` and a fresh consent queue, at the remote
/// owner's identity.
struct Session {
    explicit Session(const CapabilityDecisionTable& decisions)
        : policy(decisions)
    {
        policy.setGrantStore(&store);
        policy.setConsentQueue(&queue);
        policy.setCurrentIdentity(remoteIdentity);
    }

    CapabilityDecision check(Capability capability, CapabilityOrigin origin) { return policy.check(capability, origin, ""); }

    CapabilityGrantStore store;
    CapabilityConsentQueue queue;
    HostCapabilityPolicy policy;
};

} // namespace

// The default: a remote program's own presentation needs no grant and asks
// nobody.
TEST(testRemoteInformationalEffectsAreAllowedByDefault)
{
    Session session(decisionsFrom(""));
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    CHECK(session.check(Capability::SetCursorShape, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    CHECK(!session.store.isGranted(Capability::SetHyperlink, CapabilityOrigin::Remote, remoteIdentity));
    CHECK(session.queue.takeNewlyPending().empty());

    // The policy a session gets with no configuration at all is the same table.
    CHECK(HostCapabilityPolicy {}.decisions() == decisionsFrom(""));
}

// Restricted to prompt: the same checks return Prompt, which Core treats as a
// refusal until a human grants, and queue one question however many times the
// program repeats them.
TEST(testRemoteInformationalRestrictedToPromptAsksOnce)
{
    Session session(decisionsFrom("[capabilities.remote]\ninformational = \"prompt\"\n"));
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    const auto asked = session.queue.takeNewlyPending();
    CHECK_EQ(static_cast<int>(asked.size()), 1);
    if (!asked.empty()) {
        CHECK(asked[0].capability == Capability::SetHyperlink);
        CHECK(asked[0].origin == CapabilityOrigin::Remote);
        CHECK(asked[0].identity == remoteIdentity);
    }

    CHECK(session.check(Capability::SetCursorShape, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK_EQ(static_cast<int>(session.queue.takeNewlyPending().size()), 1);

    // Only Remote was restricted.
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Agent) == CapabilityDecision::Allow);
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Local) == CapabilityDecision::Allow);
}

// Restricted to deny: refused, and nobody is asked.
TEST(testRemoteInformationalRestrictedToDenyAsksNobody)
{
    Session session(decisionsFrom("[capabilities.remote]\ninformational = \"deny\"\n"));
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(session.check(Capability::SetCursorShape, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(session.queue.takeNewlyPending().empty());
}

// The trust property. Allowing the informational tier moves nothing else: the
// host-escaping and exfil effects a remote program reaches stay denied under the
// default file, and a file that names only the informational tier leaves them
// where they were. The two host-escaping effects the owner asks for by default
// are the exception, and they are asked about rather than allowed.
TEST(testRemoteHostEscapingAndExfiltrationStayDenied)
{
    for (const char* text : { "", "[capabilities.remote]\ninformational = \"allow\"\n" }) {
        Session session(decisionsFrom(text));
        for (const Capability capability : { Capability::ExecuteTrigger, Capability::FileReadAccess, Capability::ProgramKeys, Capability::WindowMutate, Capability::PlaySound, Capability::ClipboardRead, Capability::ScreenContentRead, Capability::WindowTitleRead })
            CHECK_MSG(session.check(capability, CapabilityOrigin::Remote) == CapabilityDecision::Deny, text);
        CHECK_MSG(session.queue.takeNewlyPending().empty(), text);
    }
}

// Copying from a remote editor and a remote build's notification are the
// owner's own device acting for the owner, so they are asked about rather than
// refused: the first use raises one consent prompt, however often the program
// repeats it.
TEST(testRemoteClipboardWriteAndNotificationPromptByDefault)
{
    Session session(decisionsFrom(""));
    CHECK(session.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK(session.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    const auto asked = session.queue.takeNewlyPending();
    CHECK_EQ(static_cast<int>(asked.size()), 1);
    if (!asked.empty()) {
        CHECK(asked[0].capability == Capability::ClipboardWrite);
        CHECK(asked[0].origin == CapabilityOrigin::Remote);
        CHECK(asked[0].identity == remoteIdentity);
    }

    CHECK(session.check(Capability::DesktopNotification, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK_EQ(static_cast<int>(session.queue.takeNewlyPending().size()), 1);

    CHECK(defaultDecision(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    CHECK(defaultDecision(Capability::DesktopNotification, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
    // The tier's own default is unchanged: the refinement names two members.
    static_assert(defaultDecision(CapabilityTier::HostEscaping, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
}

// A grant answers the prompt, for whatever lifetime the human chose.
TEST(testARemoteClipboardWriteGrantAllows)
{
    for (const CapabilityGrantScope scope : { CapabilityGrantScope::OneShot, CapabilityGrantScope::Session, CapabilityGrantScope::Always }) {
        Session session(decisionsFrom(""));
        session.store.grant(Capability::ClipboardWrite, CapabilityOrigin::Remote, remoteIdentity, scope, 1);
        CHECK(session.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
        CHECK(session.queue.takeNewlyPending().empty());
        if (scope != CapabilityGrantScope::OneShot)
            CHECK(session.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    }
}

// The owner's written restriction outranks the refinement: a configured deny
// for the host-escaping tier is hard for these two as for the rest of it, even
// against a grant persisted before the file said so, and a configured allow
// needs no grant.
TEST(testConfiguredRemoteHostEscapingOutranksTheRefinement)
{
    Session denied(decisionsFrom("[capabilities.remote]\nhost_escaping = \"deny\"\n"));
    denied.store.grant(Capability::ClipboardWrite, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::Always);
    denied.store.grant(Capability::DesktopNotification, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::Always);
    CHECK(denied.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(denied.check(Capability::DesktopNotification, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(denied.store.isGranted(Capability::ClipboardWrite, CapabilityOrigin::Remote, remoteIdentity));
    CHECK(denied.queue.takeNewlyPending().empty());

    Session allowed(decisionsFrom("[capabilities.remote]\nhost_escaping = \"allow\"\n"));
    CHECK(allowed.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    CHECK(allowed.check(Capability::DesktopNotification, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    CHECK(allowed.queue.takeNewlyPending().empty());

    // Spelling out the tier's default is a restriction too: the file named it.
    CHECK(decisionsFrom("[capabilities.remote]\nhost_escaping = \"deny\"\n") != decisionsFrom(""));
}

// The refinement is the remote origin's alone: every capability the agent and
// local origins reach keeps its tier's verdict.
TEST(testRefinementLeavesTheOtherOriginsAlone)
{
    constexpr CapabilityDecisionTable defaults;
    for (std::uint16_t code = 0; code <= std::to_underlying(Capability::None); ++code) {
        const auto capability = static_cast<Capability>(code);
        for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent })
            CHECK(defaults.decision(origin, capability) == defaultDecision(tierOf(capability), origin));
    }
}

// Benign and Rendering are Allow for every origin and no configuration says
// otherwise: the file has no key for them, and a table cannot be made to hold
// anything else for them.
TEST(testBenignAndRenderingCannotBeConfigured)
{
    for (const char* key : { "benign", "rendering" }) {
        Preferences::Values values;
        std::istringstream input(std::string("[capabilities.remote]\n") + key + " = \"deny\"\n");
        CHECK_MSG(!Preferences::parse(input, values, Preferences::ParseMode::Strict), key);
        Session session(Preferences::Effective::capabilityDecisions(values));
        CHECK_MSG(session.check(Capability::DeviceAttributesRead, CapabilityOrigin::Remote) == CapabilityDecision::Allow, key);
        CHECK_MSG(session.check(Capability::ImageRender, CapabilityOrigin::Remote) == CapabilityDecision::Allow, key);
    }

    // The table itself refuses the cell, whoever asks.
    constexpr CapabilityDecisionTable defaults;
    for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        for (const CapabilityTier tier : { CapabilityTier::Benign, CapabilityTier::Rendering }) {
            for (const CapabilityDecision decision : { CapabilityDecision::Deny, CapabilityDecision::Prompt }) {
                const CapabilityDecisionTable attempted = defaults.replacing(origin, tier, decision);
                CHECK(attempted == defaults);
                CHECK(attempted.decision(origin, tier) == CapabilityDecision::Allow);
            }
        }
    }
    static_assert(CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, CapabilityTier::Benign, CapabilityDecision::Deny).decision(CapabilityOrigin::Remote, CapabilityTier::Benign) == CapabilityDecision::Allow);
}

// The agent origin keeps its defaults, and its own table is honoured.
TEST(testAgentDefaultsAndOverride)
{
    Session defaults(decisionsFrom(""));
    CHECK(defaults.check(Capability::ClipboardWrite, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);
    CHECK(defaults.check(Capability::ScreenContentRead, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);
    CHECK(defaults.check(Capability::SetHyperlink, CapabilityOrigin::Agent) == CapabilityDecision::Allow);

    Session restricted(decisionsFrom("[capabilities.agent]\nhost_escaping = \"deny\"\n"));
    CHECK(restricted.check(Capability::ClipboardWrite, CapabilityOrigin::Agent) == CapabilityDecision::Deny);
    CHECK(restricted.queue.takeNewlyPending().empty());
    // The rest of the agent's table, and the other origins, are untouched.
    CHECK(restricted.check(Capability::ScreenContentRead, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);
    CHECK(restricted.check(Capability::SetHyperlink, CapabilityOrigin::Agent) == CapabilityDecision::Allow);
    CHECK(restricted.check(Capability::ClipboardWrite, CapabilityOrigin::Local) == CapabilityDecision::Allow);
    CHECK(restricted.check(Capability::ClipboardWrite, CapabilityOrigin::Remote) == CapabilityDecision::Prompt);
}

// A standing grant answers a Prompt cell, as it does over a default Prompt.
TEST(testAGrantAnswersARestrictedPrompt)
{
    Session session(decisionsFrom("[capabilities.remote]\ninformational = \"prompt\"\n"));
    session.store.grant(Capability::SetHyperlink, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::Session, 1);
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Allow);
    CHECK(session.queue.takeNewlyPending().empty());
}

// A Deny cell is hard: no grant of any scope lifts it, and a OneShot grant is
// not consumed by the refusal. The same holds for a default Deny.
TEST(testNoGrantLiftsADeny)
{
    Session session(decisionsFrom("[capabilities.remote]\ninformational = \"deny\"\n"));
    session.store.grant(Capability::SetHyperlink, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::Session, 1);
    session.store.grant(Capability::SetCursorShape, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::OneShot);
    session.store.grant(Capability::ExecuteTrigger, CapabilityOrigin::Remote, remoteIdentity, CapabilityGrantScope::Always);
    CHECK(session.check(Capability::SetHyperlink, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(session.check(Capability::SetCursorShape, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(session.store.isGranted(Capability::SetCursorShape, CapabilityOrigin::Remote, remoteIdentity));
    CHECK(session.check(Capability::ExecuteTrigger, CapabilityOrigin::Remote) == CapabilityDecision::Deny);
    CHECK(session.queue.takeNewlyPending().empty());
}

// The clipboard read is exfiltration, not a host escape: it answers to the
// read_exfiltration key and not to host_escaping.
TEST(testClipboardReadIsReadExfiltration)
{
    static_assert(tierOf(Capability::ClipboardRead) == CapabilityTier::ReadExfil);
    static_assert(tierOf(Capability::ClipboardWrite) == CapabilityTier::HostEscaping);

    Session exfiltration(decisionsFrom("[capabilities.agent]\nread_exfiltration = \"deny\"\n"));
    CHECK(exfiltration.check(Capability::ClipboardRead, CapabilityOrigin::Agent) == CapabilityDecision::Deny);
    CHECK(exfiltration.check(Capability::ClipboardWrite, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);

    Session escaping(decisionsFrom("[capabilities.agent]\nhost_escaping = \"deny\"\n"));
    CHECK(escaping.check(Capability::ClipboardRead, CapabilityOrigin::Agent) == CapabilityDecision::Prompt);
    CHECK(escaping.check(Capability::ClipboardWrite, CapabilityOrigin::Agent) == CapabilityDecision::Deny);
}

// One origin's verdict for one capability, read without building the whole
// table, is the table's answer for that pair, for every capability and origin
// under the default file and under overrides of every configurable tier, and
// so is one tier's verdict; every reader refuses a value from outside the
// enumerations: one rule answers them all.
TEST(testOneVerdictIsTheTablesAnswer)
{
    for (const std::string& text : { std::string(), std::string("[capabilities.remote]\nhost_escaping = \"deny\"\nread_exfiltration = \"prompt\"\ninformational = \"deny\"\n"),
             std::string("[capabilities.remote]\nhost_escaping = \"allow\"\n[capabilities.agent]\nhost_escaping = \"deny\"\n[capabilities.local]\ninformational = \"prompt\"\n") }) {
        Preferences::Values values;
        std::istringstream input(text);
        CHECK_MSG(Preferences::parse(input, values, Preferences::ParseMode::Strict), text.c_str());
        const CapabilityDecisionTable table = Preferences::Effective::capabilityDecisions(values);
        for (std::uint16_t code = 0; code <= std::to_underlying(Capability::None); ++code) {
            const auto capability = static_cast<Capability>(code);
            for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote })
                CHECK(Preferences::Effective::capabilityDecision(values, origin, capability) == table.decision(origin, capability));
        }
        for (std::uint8_t tier = 0; tier <= std::to_underlying(CapabilityTier::Rendering); ++tier) {
            for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote })
                CHECK(Preferences::Effective::capabilityDecision(values, origin, static_cast<CapabilityTier>(tier)) == table.decision(origin, static_cast<CapabilityTier>(tier)));
        }
        // A value cast in from outside the enumerations is refused by every
        // reader, whatever the strictest origin's cells say.
        const auto outside = static_cast<CapabilityOrigin>(9);
        CHECK(Preferences::Effective::capabilityDecision(values, outside, Capability::ClipboardWrite) == CapabilityDecision::Deny);
        CHECK(table.decision(outside, Capability::ClipboardWrite) == CapabilityDecision::Deny);
        for (const auto tier : { CapabilityTier::HostEscaping, CapabilityTier::LowRiskInfo, CapabilityTier::Benign }) {
            CHECK(Preferences::Effective::capabilityDecision(values, outside, tier) == CapabilityDecision::Deny);
            CHECK(table.decision(outside, tier) == CapabilityDecision::Deny);
        }
        CHECK(Preferences::Effective::capabilityDecision(values, CapabilityOrigin::Remote, static_cast<CapabilityTier>(9)) == CapabilityDecision::Deny);
        CHECK(table.decision(CapabilityOrigin::Remote, static_cast<CapabilityTier>(9)) == CapabilityDecision::Deny);
    }
}

UBIQUITTY_TEST_MAIN()
