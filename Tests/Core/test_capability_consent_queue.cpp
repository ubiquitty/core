// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_capability_consent_queue — the Prompt sink of the consent-PROMPT flow
// When HostCapabilityPolicy::check() resolves to Prompt with no standing
// grant, it records the (capability, origin, identity) triple into the
// CapabilityConsentQueue so the daemon can ask the human once and, on approval,
// record a grant. This unit-tests that sink directly (no Terminal):
//   - a Prompt-tier check enqueues the triple (deduped, identity-keyed)
//   - Allow / Deny tiers enqueue nothing
//   - an empty (unverified) identity is never queued (can't hold a grant)
//   - a standing grant short-circuits to Allow and enqueues nothing
//   - resolve() clears a pending entry (the human ruled)

#include "Platform/Host/HostCapabilityPolicy.h"

#include <cstdio>

using namespace Ubiquitty;

// The queue's subject is a Prompt verdict, and the remote informational tier is
// the one these cases drive, so every policy here restricts it to Prompt; its
// default is Allow.
constexpr CapabilityDecisionTable promptingRemote = CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, CapabilityTier::LowRiskInfo, CapabilityDecision::Prompt);

// The host skips its own question only for an effect its recipient judges by
// the very verdict the host would apply: a clipboard write or notification
// gated at the Remote origin. Every other origin keeps the host's question,
// with that origin's rule, an agent's single use; so does a sound, and a host
// that asks. Checked over every origin and every effect a recipient applies,
// with every cell asking, so the rule rather than a default decides.
TEST(onlyARemoteOriginsEffectIsForwardedToTheRecipientThatAsks)
{
    CapabilityDecisionTable asking;
    for (const auto origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        for (const auto tier : { CapabilityTier::HostEscaping, CapabilityTier::ReadExfil, CapabilityTier::LowRiskInfo })
            asking = asking.replacing(origin, tier, CapabilityDecision::Prompt);
    }
    for (const auto question : { HostCapabilityPolicy::EffectQuestion::AskedByRecipient, HostCapabilityPolicy::EffectQuestion::AskedByHost }) {
        for (const auto origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
            for (const auto capability : { Capability::ClipboardWrite, Capability::DesktopNotification, Capability::PlaySound, Capability::RequestAttention }) {
                CapabilityConsentQueue queue;
                HostCapabilityPolicy policy(asking);
                policy.setConsentQueue(&queue);
                policy.setCurrentIdentity("token:phone");
                policy.setEffectQuestion(question);
                const bool isForwarded = question == HostCapabilityPolicy::EffectQuestion::AskedByRecipient && origin == CapabilityOrigin::Remote
                    && (capability == Capability::ClipboardWrite || capability == Capability::DesktopNotification);
                const CapabilityDecision decision = policy.check(capability, origin, {});
                CHECK_MSG(decision == (isForwarded ? CapabilityDecision::Allow : CapabilityDecision::Prompt),
                    "an effect was forwarded unasked from an origin whose rule only the host applies, or asked about twice");
                CHECK(queue.isPending(capability, origin, "token:phone") == !isForwarded);
            }
        }
    }
    // A grant a person gave here admits the effect as this host's own, so a
    // banner is admitted, not forwarded, and is recorded for other devices.
    {
        CapabilityGrantStore store;
        HostCapabilityPolicy granted;
        granted.setGrantStore(&store);
        granted.setCurrentIdentity("token:phone");
        granted.setEffectQuestion(HostCapabilityPolicy::EffectQuestion::AskedByRecipient);
        store.grant(Capability::DesktopNotification, CapabilityOrigin::Remote, "token:phone", CapabilityGrantScope::Always);
        CHECK(granted.check(Capability::DesktopNotification, CapabilityOrigin::Remote, {}) == CapabilityDecision::Allow);
        CHECK_MSG(!granted.takeForwardedNotification(), "a banner this host's own grant admitted was taken for one forwarded unasked");
    }
    // A Deny the host's configuration says refuses even what the recipient asks about.
    HostCapabilityPolicy denying(CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, CapabilityTier::HostEscaping, CapabilityDecision::Deny));
    denying.setEffectQuestion(HostCapabilityPolicy::EffectQuestion::AskedByRecipient);
    CHECK(denying.check(Capability::ClipboardWrite, CapabilityOrigin::Remote, {}) == CapabilityDecision::Deny);
}

TEST(capabilityConsentQueue)
{

    CapabilityGrantStore store;
    CapabilityConsentQueue queue;
    HostCapabilityPolicy policy(promptingRemote);
    policy.setGrantStore(&store);
    policy.setConsentQueue(&queue);
    policy.setCurrentIdentity("agent-abc");

    // SetColor is LowRiskInfo → restricted Remote resolves to Prompt (no grant). The check
    // still RETURNS Prompt (chokepoint denies), but the triple is now queued.
    CHECK(policy.check(Capability::SetColor, CapabilityOrigin::Remote, {}) == CapabilityDecision::Prompt);
    CHECK(queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-abc"));

    // Dedup: a repeat op re-enqueues nothing new (one prompt, not a storm).
    CHECK(policy.check(Capability::SetColor, CapabilityOrigin::Remote, {}) == CapabilityDecision::Prompt);
    CHECK(!queue.recordPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-abc"));

    // Identity-keyed: a different principal is a distinct (absent) entry.
    CHECK(!queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-xyz"));

    // Allow tier enqueues nothing: Local origin allows everything.
    CHECK(policy.check(Capability::SetColor, CapabilityOrigin::Local, {}) == CapabilityDecision::Allow);
    CHECK(!queue.isPending(Capability::SetColor, CapabilityOrigin::Local, "agent-abc"));

    // Deny tier enqueues nothing: a Remote ReadExfil op is Deny, not Prompt.
    CHECK(policy.check(Capability::ScreenContentRead, CapabilityOrigin::Remote, {}) == CapabilityDecision::Deny);
    CHECK(!queue.isPending(Capability::ScreenContentRead, CapabilityOrigin::Remote, "agent-abc"));

    // An empty (unverified) identity is never queued — it could never hold a grant.
    policy.setCurrentIdentity("");
    CHECK(policy.check(Capability::SetHyperlink, CapabilityOrigin::Remote, {}) == CapabilityDecision::Prompt);
    CHECK(!queue.isPending(Capability::SetHyperlink, CapabilityOrigin::Remote, ""));
    policy.setCurrentIdentity("agent-abc");

    // A standing grant short-circuits check() to Allow → nothing newly pending.
    store.grant(Capability::SetCursorShape, CapabilityOrigin::Remote, "agent-abc", CapabilityGrantScope::Session);
    CHECK(policy.check(Capability::SetCursorShape, CapabilityOrigin::Remote, {}) == CapabilityDecision::Allow);
    CHECK(!queue.isPending(Capability::SetCursorShape, CapabilityOrigin::Remote, "agent-abc"));

    // resolve() drops a pending entry once the human has ruled.
    CHECK(queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-abc"));
    queue.resolve(Capability::SetColor, CapabilityOrigin::Remote, "agent-abc");
    CHECK(!queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-abc"));

    // takeNewlyPending: the daemon drains the entries recorded since its last
    // call — one per genuinely-new triple (a repeat op adds nothing), and a
    // second drain is empty. Isolated on fresh objects.
    {
        CapabilityConsentQueue drainQueue;
        HostCapabilityPolicy drainPolicy(promptingRemote);
        drainPolicy.setConsentQueue(&drainQueue);
        drainPolicy.setCurrentIdentity("agent-2");

        (void)drainPolicy.check(Capability::SetColor, CapabilityOrigin::Remote, {}); // Prompt → queued (new)
        (void)drainPolicy.check(Capability::SetColor, CapabilityOrigin::Remote, {}); // repeat → dedup, nothing new
        (void)drainPolicy.check(Capability::SetHyperlink, CapabilityOrigin::Remote, {}); // Prompt → queued (new)
        (void)drainPolicy.check(Capability::SetColor, CapabilityOrigin::Local, {}); // Allow → nothing queued

        std::vector<CapabilityGrantKey> drained = drainQueue.takeNewlyPending();
        CHECK(drained.size() == 2); // SetColor + SetHyperlink, once each — not the repeat, not the Allow
        CHECK(drainQueue.takeNewlyPending().empty()); // a second drain has nothing new
        // Drained entries stay pending until the human rules (no auto-resolve).
        CHECK(drainQueue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "agent-2"));
    }

    // A principal that goes away, by disconnecting or by being revoked, leaves
    // no pending entry behind. The dedup is why: a recorded triple suppresses
    // every later request for the same triple, so an entry nobody can rule on
    // denies the capability permanently rather than merely occupying memory.
    // The human is never asked again, and check() keeps returning Prompt while
    // no request is emitted. resolveIdentity is the drop, and until it is
    // called the only exit is a daemon restart.
    {
        CapabilityConsentQueue queue;
        HostCapabilityPolicy policy(promptingRemote);
        policy.setConsentQueue(&queue);
        policy.setCurrentIdentity("token:gone");

        (void)policy.check(Capability::SetColor, CapabilityOrigin::Remote, {});
        (void)policy.check(Capability::SetHyperlink, CapabilityOrigin::Remote, {});
        CHECK(queue.takeNewlyPending().size() == 2);
        CHECK(queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "token:gone"));

        // A second ask while the first is outstanding is deduped, which is the
        // behaviour that makes an unresolved entry permanent.
        (void)policy.check(Capability::SetColor, CapabilityOrigin::Remote, {});
        CHECK(queue.takeNewlyPending().empty());

        queue.resolveIdentity("token:gone");
        CHECK(!queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "token:gone"));
        CHECK(!queue.isPending(Capability::SetHyperlink, CapabilityOrigin::Remote, "token:gone"));

        // And the point of dropping it: the next ask reaches a human again.
        (void)policy.check(Capability::SetColor, CapabilityOrigin::Remote, {});
        CHECK(queue.takeNewlyPending().size() == 1);

        // One principal's departure leaves another's question standing.
        policy.setCurrentIdentity("token:stays");
        (void)policy.check(Capability::SetColor, CapabilityOrigin::Remote, {});
        CHECK(queue.takeNewlyPending().size() == 1);
        queue.resolveIdentity("token:gone");
        CHECK(queue.isPending(Capability::SetColor, CapabilityOrigin::Remote, "token:stays"));
    }
}

UBIQUITTY_TEST_MAIN()
