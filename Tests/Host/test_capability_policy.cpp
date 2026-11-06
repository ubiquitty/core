// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// capability_policy — HostCapabilityPolicy enforces the per-origin tier table
// and the (capability x origin x identity) grant store. The per-capability tier
// CLASSIFICATION is security-owned; this pins the ratified tier x origin matrix
// and the grant scoping against unambiguous tier representatives.

#include "Assertions.h"

#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdio>
#include <string>
#include <string_view>

using namespace Ubiquitty;

// A fresh per-session policy bound to the store, driven at a given identity.
static CapabilityDecision decide(CapabilityGrantStore& store, Capability capability, CapabilityOrigin origin, std::string_view identity)
{
    HostCapabilityPolicy policy;
    policy.setGrantStore(&store);
    policy.setCurrentIdentity(std::string(identity));
    return policy.check(capability, origin, "");
}

int main()
{
    // Unambiguous tier representatives (which survive a classification tune of the
    // borderline capabilities).
    constexpr Capability hostEscaping = Capability::ExecuteTrigger;
    constexpr Capability exfil = Capability::ScreenContentRead; // reads beyond the rendered screen
    constexpr Capability lowRisk = Capability::SetWorkingDirectory; // display-metadata write
    constexpr Capability benign = Capability::DeviceAttributesRead; // feature/cursor read
    constexpr Capability rendering = Capability::ImageRender;

    CapabilityGrantStore store;

    // Local trusts its own programs — Allow across tiers.
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Local, "local") == CapabilityDecision::Allow);
    CHECK(decide(store, lowRisk, CapabilityOrigin::Local, "local") == CapabilityDecision::Allow);

    // Agent: host-escaping prompts (deny-for-now); low-risk + rendering allowed.
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Prompt);
    CHECK(decide(store, lowRisk, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Allow);
    CHECK(decide(store, rendering, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Allow);

    // Remote: host-escaping denied; low-risk informational and rendering allowed.
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Deny);
    CHECK(decide(store, lowRisk, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Allow);
    CHECK(decide(store, rendering, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Allow);

    // Exfil reads (ScreenContentRead/WindowTitleRead read beyond the rendered
    // screen) — Remote denied, Agent prompts, Local allowed.
    CHECK(decide(store, exfil, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Deny);
    CHECK(decide(store, exfil, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Prompt);
    CHECK(decide(store, exfil, CapabilityOrigin::Local, "local") == CapabilityDecision::Allow);

    // Benign feature/cursor reads + None — allowed at every origin (programs block
    // on them; they reveal nothing past the screen the peer already sees). The
    // read/write asymmetry: SetWindowTitle (Benign) writes a title a Remote can't
    // read back (WindowTitleRead stays Deny).
    CHECK(decide(store, benign, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Allow);
    CHECK(decide(store, benign, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Allow);
    CHECK(decide(store, Capability::None, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Allow);
    CHECK(decide(store, Capability::SetWindowTitle, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Allow);
    CHECK(decide(store, Capability::WindowTitleRead, CapabilityOrigin::Remote, "remoteX") == CapabilityDecision::Deny);

    // A grant flips the default to Allow for the EXACT (capability, origin, identity).
    // Session scope: the grant persists until dropSession (or explicit revoke here).
    store.grant(hostEscaping, CapabilityOrigin::Agent, "agentA", CapabilityGrantScope::Session, 1);
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Allow);

    // Grant scoping: a different identity / origin / capability does not match.
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentB") == CapabilityDecision::Prompt);
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Remote, "agentA") == CapabilityDecision::Deny);
    CHECK(decide(store, Capability::FileReadAccess, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Prompt);

    // An unverified principal (empty identity) never holds a grant.
    store.grant(hostEscaping, CapabilityOrigin::Remote, "", CapabilityGrantScope::Session, 1);
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Remote, "") == CapabilityDecision::Deny);

    // Revoke drops the grant back to the default.
    store.revoke(hostEscaping, CapabilityOrigin::Agent, "agentA");
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Prompt);

    // revokeIdentity drops every grant for a principal, leaving others.
    store.grant(hostEscaping, CapabilityOrigin::Agent, "agentA", CapabilityGrantScope::Session, 1);
    store.grant(lowRisk, CapabilityOrigin::Remote, "agentA", CapabilityGrantScope::Session, 1);
    store.grant(hostEscaping, CapabilityOrigin::Agent, "agentB", CapabilityGrantScope::Session, 1);
    store.revokeIdentity("agentA");
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentA") == CapabilityDecision::Prompt);
    CHECK(decide(store, hostEscaping, CapabilityOrigin::Agent, "agentB") == CapabilityDecision::Allow);

    if (failures == 0)
        printf("OK\n");
    return failures ? 1 : 0;
}
