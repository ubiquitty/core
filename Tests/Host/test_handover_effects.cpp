// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"
#include "Platform/Host/HandoverEffects.h"

using namespace Ubiquitty;
namespace Host = Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

constexpr Host::HandoverEffectsLimits limits { 4 * 1024 * 1024, 4096, 65536, 2 * 1024 * 1024 };
constexpr Sync::ID128 recipient { 13, 27 };

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}

void prepare(HostCapabilityPolicy& policy, Host::HostEffectQueue& effects)
{
    policy.setCurrentIdentity("token:agent-first");
    constexpr std::array buffers { ClipboardBuffer::Clipboard, ClipboardBuffer::Primary };
    const std::string bytes("image\0retained", 14);
    policy.holdClipboardWrite(CapabilityOrigin::Agent, buffers, bytes);
    policy.pendingClipboardWrite()->requestID = 41;
    // The current input principal may have changed while the first person is
    // deciding. That must not retarget the held operation.
    policy.setCurrentIdentity("token:human-next");
    effects.push(Host::HostNotification { "build", "finished", "job-42", NotificationOccasion::Invisible, recipient, 91 });
    Host::HostClipboardClient clipboard(effects);
    clipboard.writeClipboard(ClipboardBuffer::Primary, bytes);
    auto owners = effects.take();
    auto& write = std::get<Host::HostEffectQueue::ClipboardWrite>(owners.back());
    write.primary = Sync::ID128 { 31, 53 };
    write.pendingPrimaryClientID = 82;
    for (auto& owner : owners)
        effects.returnUndelivered(std::move(owner));
}

} // namespace

TEST(heldConsentAndDeliveryRetainTheirExactOwners)
{
    const auto decisions = CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, CapabilityTier::HostEscaping, CapabilityDecision::Deny);
    HostCapabilityPolicy policy(decisions);
    Host::HostEffectQueue effects;
    prepare(policy, effects);
    const auto* originalBytes = policy.pendingClipboardWrite()->bytes.get();
    auto encoded = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(encoded);
    auto decoded = Host::decodeHandoverEffects(*encoded, limits);
    REQUIRE(decoded);
    CHECK(decoded->policy.decisions == decisions);
    CHECK(decoded->policy.currentIdentity == "token:human-next");
    const auto& held = decoded->policy.pendingClipboardWrite;
    REQUIRE(held);
    CHECK(held->identity == "token:agent-first");
    CHECK(held->origin == CapabilityOrigin::Agent);
    CHECK_EQ(held->requestID, 41u);
    CHECK_EQ(held->bufferCount, 2u);
    CHECK(held->buffers[1] == ClipboardBuffer::Primary);
    CHECK(std::string_view(held->bytes.get(), held->byteCount) == std::string_view("image\0retained", 14));
    CHECK(held->bytes.get() != originalBytes);
    CHECK_EQ(effects.heldEffects().size(), 2u);
    CHECK(policy.pendingClipboardWrite()->bytes.get() == originalBytes);
    REQUIRE(decoded->effects.size() == 2);
    const auto& notification = std::get<Host::HostNotification>(decoded->effects[0]);
    CHECK(notification.primary == recipient);
    CHECK_EQ(notification.pendingPrimaryClientID, 91u);
    CHECK(notification.occasion == NotificationOccasion::Invisible);
    CHECK(notification.identifier == "job-42");
    const auto& write = std::get<Host::HostEffectQueue::ClipboardWrite>(decoded->effects[1]);
    CHECK((write.primary == Sync::ID128 { 31, 53 }));
    CHECK_EQ(write.pendingPrimaryClientID, 82u);
    CHECK(write.buffer == ClipboardBuffer::Primary);
    const auto clipboard = Sync::decodeEffectClipboard(write.encoded.view(), Sync::maxEffectClipboardWriteBytes);
    REQUIRE(clipboard);
    CHECK_EQ(clipboard->count, 1u);
    CHECK_EQ(clipboard->items[0].data.size(), 14u);
}

TEST(restorationDoesNotReissueNotificationsOrConsumeGrants)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    prepare(policy, effects);
    auto encoded = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(encoded);
    auto decoded = Host::decodeHandoverEffects(*encoded, limits);
    REQUIRE(decoded);
    CapabilityGrantStore grants;
    CapabilityConsentQueue consent;
    grants.grant(Capability::ClipboardWrite, CapabilityOrigin::Agent, "token:agent-first", CapabilityGrantScope::OneShot);
    HostCapabilityPolicy successor(decoded->policy.decisions);
    successor.setGrantStore(&grants);
    successor.setConsentQueue(&consent);
    CHECK(successor.restoreContinuation(std::move(decoded->policy)));
    CHECK(grants.isGranted(Capability::ClipboardWrite, CapabilityOrigin::Agent, "token:agent-first"));
    CHECK(!consent.isPending(Capability::ClipboardWrite, CapabilityOrigin::Agent, "token:agent-first"));
    CHECK(!successor.takeClipboardWrite(42));
    auto exact = successor.takeClipboardWrite(41);
    REQUIRE(exact);
    CHECK(exact->identity == "token:agent-first");
    CHECK(successor.checkFor(Capability::ClipboardWrite, exact->origin, exact->identity) == CapabilityDecision::Allow);
    CHECK(!grants.isGranted(Capability::ClipboardWrite, CapabilityOrigin::Agent, "token:agent-first"));
    Host::HostEffectQueue restored;
    int recordings = 0;
    restored.setNotificationRecorder([&](const auto&) { ++recordings; });
    CHECK(restored.restoreHeldEffects(std::move(decoded->effects)));
    CHECK_EQ(recordings, 0);
    CHECK_EQ(restored.take().size(), 2u);
    CHECK(restored.isEmpty());
    restored.push(Host::HostNotification { "later", "new", {}, NotificationOccasion::Always, std::nullopt });
    CHECK_EQ(recordings, 1);
}

TEST(unsetTierDefaultsDoNotBecomeExplicitOverrides)
{
    Host::HostEffectQueue effects;
    for (const auto origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        for (const auto tier : { CapabilityTier::HostEscaping, CapabilityTier::ReadExfil, CapabilityTier::LowRiskInfo }) {
            for (const auto verdict : { CapabilityDecision::Deny, CapabilityDecision::Allow, CapabilityDecision::Prompt }) {
                const auto decisions = CapabilityDecisionTable {}.replacing(origin, tier, verdict);
                HostCapabilityPolicy policy(decisions);
                auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
                REQUIRE(bytes);
                auto state = Host::decodeHandoverEffects(*bytes, limits);
                REQUIRE(state);
                CHECK(state->policy.decisions == decisions);
                for (unsigned capability = 0; capability <= std::to_underlying(Capability::None); ++capability)
                    CHECK(state->policy.decisions.decision(origin, static_cast<Capability>(capability)) == decisions.decision(origin, static_cast<Capability>(capability)));
            }
        }
    }
}

TEST(refusedClipboardCaptureFlagIsCarriedWithoutBeingConsumed)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    constexpr std::array buffers { ClipboardBuffer::Clipboard };
    policy.holdClipboardWrite(CapabilityOrigin::Agent, buffers, {}); // no verified identity
    REQUIRE(policy.hasClipboardWriteRefusal());
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    auto state = Host::decodeHandoverEffects(*bytes, limits);
    REQUIRE(state);
    CHECK(state->policy.wasClipboardWriteRefused);
    CHECK(!state->policy.pendingClipboardWrite);
    CHECK(policy.hasClipboardWriteRefusal());
    HostCapabilityPolicy restored(state->policy.decisions);
    CHECK(restored.restoreContinuation(std::move(state->policy)));
    CHECK(restored.takeClipboardWriteRefusal());
    CHECK(!restored.takeClipboardWriteRefusal());
}

TEST(everyTruncatedRecordAndChangedDigestIsRefused)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    prepare(policy, effects);
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    for (std::size_t length = 0; length < bytes->size(); ++length)
        CHECK(!Host::decodeHandoverEffects(std::span(*bytes).first(length), limits));
    (*bytes)[15] ^= 0x80;
    CHECK(!Host::decodeHandoverEffects(*bytes, limits));
    CHECK_EQ(effects.heldEffects().size(), 2u);
    CHECK_EQ(policy.pendingClipboardWrite()->requestID, 41u);
}

TEST(recomputedDigestDoesNotAdmitInvalidFields)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    for (const std::size_t offset : { 2u, 19u, 20u, 21u }) {
        auto malformed = *bytes;
        malformed[offset] = 255; // decision, refusal boolean, pending boolean, effect count
        redigest(malformed);
        CHECK(!Host::decodeHandoverEffects(malformed, limits));
    }
    auto length = *bytes;
    length[11] = 255; // current identity length with no backing bytes
    redigest(length);
    CHECK(!Host::decodeHandoverEffects(length, limits));
    auto newer = *bytes;
    newer[0] = 2;
    CHECK(Host::decodeHandoverEffects(newer, limits).error() == Host::HandoverEffectsError::NewerVersion);
}

TEST(receiverLimitsRefuseBeforeOwnershipMoves)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    prepare(policy, effects);
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    for (std::size_t Host::HandoverEffectsLimits::* field : { &Host::HandoverEffectsLimits::maxSectionBytes,
             &Host::HandoverEffectsLimits::maxIdentityBytes, &Host::HandoverEffectsLimits::maxNotificationBytes,
             &Host::HandoverEffectsLimits::maxClipboardBytes }) {
        auto small = limits;
        small.*field = 1;
        CHECK(!Host::encodeHandoverEffects(policy, effects, small));
        CHECK(!Host::decodeHandoverEffects(*bytes, small));
    }
    CHECK_EQ(effects.heldEffects().size(), 2u);
    CHECK_EQ(policy.pendingClipboardWrite()->requestID, 41u);
}

TEST(restoreRefusesNonemptyAndDifferentPolicyDestinations)
{
    HostCapabilityPolicy policy;
    Host::HostEffectQueue effects;
    prepare(policy, effects);
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    auto state = Host::decodeHandoverEffects(*bytes, limits);
    REQUIRE(state);
    CHECK(!policy.restoreContinuation(std::move(state->policy)));
    CHECK(state->policy.currentIdentity == "token:human-next");
    HostCapabilityPolicy denied(CapabilityDecisionTable {}.replacing(CapabilityOrigin::Agent, CapabilityTier::HostEscaping, CapabilityDecision::Deny));
    CHECK(!denied.restoreContinuation(std::move(state->policy)));
    CHECK(!effects.restoreHeldEffects(std::move(state->effects)));
    CHECK_EQ(state->effects.size(), 2u);
}

TEST(binaryIdentityAndEmptyClipboardRemainExact)
{
    HostCapabilityPolicy policy;
    policy.setCurrentIdentity(std::string("path\xff\0tail", 10));
    Host::HostEffectQueue effects;
    Host::HostClipboardClient clipboard(effects);
    clipboard.writeClipboard(ClipboardBuffer::Clipboard, {});
    auto bytes = Host::encodeHandoverEffects(policy, effects, limits);
    REQUIRE(bytes);
    auto state = Host::decodeHandoverEffects(*bytes, limits);
    REQUIRE(state);
    CHECK(state->policy.currentIdentity == policy.currentIdentity());
    REQUIRE(state->effects.size() == 1);
    const auto& write = std::get<Host::HostEffectQueue::ClipboardWrite>(state->effects.front());
    auto decoded = Sync::decodeEffectClipboard(write.encoded.view(), Sync::maxEffectClipboardWriteBytes);
    REQUIRE(decoded);
    CHECK_EQ(decoded->count, 0u);
}

UBIQUITTY_TEST_MAIN()
