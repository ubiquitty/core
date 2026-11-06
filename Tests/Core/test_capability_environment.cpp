// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Agent/AgentCapabilityPolicy.h"
#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

using namespace Ubiquitty;

namespace {

constexpr std::string_view requester = "token:test-requester";

// Test-only permission for one query on the terminal that owns this policy.
// Every other capability and origin remains denied.
class ScreenQueryPolicy final : public CapabilityPolicy {
public:
    CapabilityDecision check(Capability capability, CapabilityOrigin origin, std::string_view) override
    {
        return capability == Capability::ScreenContentRead && origin == CapabilityOrigin::Remote
            ? CapabilityDecision::Allow
            : CapabilityDecision::Deny;
    }
};

} // namespace

TEST(theEnvironmentCannotRelaxTheAgentPolicy)
{
    Agent::AgentCapabilityPolicy agentPolicy;
    for (unsigned code = 0; code <= std::to_underlying(Capability::None); ++code) {
        const auto capability = static_cast<Capability>(code);
        for (auto origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote })
            CHECK(agentPolicy.check(capability, origin, {}) == CapabilityDecision::Deny);
    }
}

TEST(theEnvironmentCannotOverrideAHardDenyEvenWithAStoredGrant)
{
    const auto decisions = CapabilityDecisionTable {}.replacing(
        CapabilityOrigin::Agent, CapabilityTier::HostEscaping, CapabilityDecision::Deny);
    CapabilityGrantStore store;
    CapabilityConsentQueue queue;
    HostCapabilityPolicy policy(decisions);
    policy.setGrantStore(&store);
    policy.setConsentQueue(&queue);
    store.grant(Capability::ClipboardWrite, CapabilityOrigin::Agent, requester, CapabilityGrantScope::OneShot);
    store.grant(Capability::FileReadAccess, CapabilityOrigin::Remote, requester, CapabilityGrantScope::Always);
    CHECK(policy.checkFor(Capability::ClipboardWrite, CapabilityOrigin::Agent, requester) == CapabilityDecision::Deny);
    CHECK(policy.checkFor(Capability::FileReadAccess, CapabilityOrigin::Remote, requester) == CapabilityDecision::Deny);
    CHECK(store.isGranted(Capability::ClipboardWrite, CapabilityOrigin::Agent, requester));
    CHECK(queue.takeNewlyPending().empty());
}

TEST(theEnvironmentCannotAnswerConsentOrExtendAOneShotGrant)
{
    CapabilityGrantStore store;
    CapabilityConsentQueue queue;
    HostCapabilityPolicy policy;
    policy.setGrantStore(&store);
    policy.setConsentQueue(&queue);
    policy.setCurrentIdentity(std::string(requester));
    CHECK(policy.check(Capability::ClipboardWrite, CapabilityOrigin::Remote, {}) == CapabilityDecision::Prompt);
    const auto asked = queue.takeNewlyPending();
    CHECK_EQ(asked.size(), 1u);
    if (!asked.empty()) {
        CHECK(asked.front().capability == Capability::ClipboardWrite);
        CHECK(asked.front().origin == CapabilityOrigin::Remote);
        CHECK(asked.front().identity == requester);
    }
    store.grant(Capability::ClipboardWrite, CapabilityOrigin::Remote, requester, CapabilityGrantScope::OneShot);
    CHECK(policy.checkFor(Capability::ClipboardWrite, CapabilityOrigin::Remote, "token:other") == CapabilityDecision::Prompt);
    CHECK(policy.check(Capability::ClipboardWrite, CapabilityOrigin::Remote, {}) == CapabilityDecision::Allow);
    CHECK(!store.isGranted(Capability::ClipboardWrite, CapabilityOrigin::Remote, requester));
    CHECK(policy.check(Capability::ClipboardWrite, CapabilityOrigin::Remote, {}) == CapabilityDecision::Prompt);
    CHECK(policy.check(Capability::DeviceAttributesRead, CapabilityOrigin::Remote, {}) == CapabilityDecision::Allow);
}

TEST(aTestPolicyAllowsOnlyItsNamedQueryOnItsOwningTerminal)
{
    ScreenQueryPolicy testPolicy;
    HostCapabilityPolicy productionPolicy;
    Terminal permitted;
    Terminal neighbor;
    hardReset(permitted);
    hardReset(neighbor);
    resize(permitted, 5, 40);
    resize(neighbor, 5, 40);
    permitted.capabilityPolicy = &testPolicy;
    neighbor.capabilityPolicy = &productionPolicy;
    permitted.currentCapabilityOrigin = CapabilityOrigin::Remote;
    neighbor.currentCapabilityOrigin = CapabilityOrigin::Remote;
    constexpr std::string_view query = "\033[12;0;1;1;1;1*y";
    UbiquittyTest::feed(permitted, query);
    UbiquittyTest::feed(neighbor, query);
    char replies[512];
    CHECK(UbiquittyTest::drain(permitted, replies) > 0);
    CHECK_EQ(UbiquittyTest::drain(neighbor, replies), 0);
    CHECK(testPolicy.check(Capability::ClipboardRead, CapabilityOrigin::Remote, {}) == CapabilityDecision::Deny);
    CHECK(testPolicy.check(Capability::ScreenContentRead, CapabilityOrigin::Agent, {}) == CapabilityDecision::Deny);
}

int main(int argc, char** argv)
{
    // Set before any policy query so process-local environment caching cannot
    // make a negative regression vacuous. Each test process is isolated.
    if (::setenv("UBIQUITTY_UNATTENDED", "1", 1) != 0)
        return 2;
    UBIQUITTY_TEST_PIN_PREFERENCES();
    return UbiquittyTest::runRegisteredCases(argc, argv);
}
