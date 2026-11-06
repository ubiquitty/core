// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"
#include "Platform/Host/HandoverAuthorityTables.h"

#include <algorithm>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;
namespace Sync = Protocol::Sync;

namespace {
constexpr HandoverAuthorityLimits limits { .maxSectionBytes = 65536, .maxGrants = 64, .maxConsentEntries = 128, .maxQUICTokens = 64, .maxStandingAgents = 64, .maxStringBytes = 16384 };
const CapabilityGrantKey key { Capability::SetFont, CapabilityOrigin::Agent, "token:stable" };

HandoverAuthorityTables state()
{
    HandoverAuthorityTables result;
    result.decisions = result.decisions.replacing(CapabilityOrigin::Remote, CapabilityTier::HostEscaping, CapabilityDecision::Deny)
                           .replacing(CapabilityOrigin::Agent, CapabilityTier::ReadExfil, CapabilityDecision::Prompt);
    result.grants = { { key, { CapabilityGrantScope::OneShot, 0 } },
        { { Capability::SetWindowTitle, CapabilityOrigin::Remote, "token:other" }, { CapabilityGrantScope::Session, 7 } } };
    result.pendingConsent = { key };
    result.newlyPendingConsent = { key, key, { Capability::PlaySound, CapabilityOrigin::Local, "resolved" } };
    const std::string token(32, '\0');
    result.quicTokens = { { token, 50000, 71, Protocol::Sync::Scope::Observe | Protocol::Sync::Scope::Drive, Crypto::sha256Hex(token), true } };
    result.alwaysAllowedAgents = { "token:standing", "/trusted/agent" };
    result.nextConsentRequestID = 23;
    result.nextClipboardRequestID = 118;
    result.isTokenRequired = true;
    result.didFailManagementHistory = true;
    return result;
}

std::vector<std::uint8_t> bytesOf(const Sync::HandoverBytes& bytes) { return { bytes.view().begin(), bytes.view().end() }; }

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}
} // namespace

TEST(authorityRoundTripPreservesDefaultsSecretsAndAbsoluteClocks)
{
    const auto original = state();
    const auto encoded = encodeHandoverAuthority(original, limits);
    REQUIRE(encoded);
    const auto restored = decodeHandoverAuthority(encoded->view(), limits);
    REQUIRE(restored);
    CHECK(restored->decisions == original.decisions);
    CHECK(!restored->decisions.configuredDecision(CapabilityOrigin::Local, CapabilityTier::ReadExfil));
    CHECK(restored->decisions.configuredDecision(CapabilityOrigin::Remote, CapabilityTier::HostEscaping) == CapabilityDecision::Deny);
    CHECK(restored->pendingConsent == original.pendingConsent);
    CHECK(restored->newlyPendingConsent == original.newlyPendingConsent);
    CHECK_EQ(restored->nextConsentRequestID, 23u);
    CHECK_EQ(restored->nextClipboardRequestID, 118u);
    CHECK_EQ(restored->quicTokens[0].deadlineMilliseconds, 50000u);
    CHECK_EQ(restored->quicTokens[0].mintingClientID, 71u);
    CHECK(restored->quicTokens[0].token == original.quicTokens[0].token);
    CHECK(restored->quicTokens[0].scope == original.quicTokens[0].scope);
    CHECK(restored->quicTokens[0].tokenID == original.quicTokens[0].tokenID);
    CHECK(restored->quicTokens[0].isInteractiveUser);
    CHECK(restored->alwaysAllowedAgents == original.alwaysAllowedAgents);
    CHECK(restored->isTokenRequired && restored->didFailManagementHistory);
    const auto repeated = encodeHandoverAuthority(*restored, limits);
    REQUIRE(repeated);
    CHECK(bytesOf(*repeated) == bytesOf(*encoded));
}

TEST(importedGrantsStaySingleUseAndBoundToOriginalPrincipalAndSession)
{
    const auto original = state();
    const auto encoded = encodeHandoverAuthority(original, limits);
    REQUIRE(encoded);
    const auto restored = decodeHandoverAuthority(encoded->view(), limits);
    REQUIRE(restored);
    CapabilityGrantStore store;
    for (const auto& [grantKey, record] : restored->grants)
        store.grant(grantKey.capability, grantKey.origin, grantKey.identity, record.scope, record.sessionID);
    CHECK(!store.checkAndConsumeIfGranted(key.capability, key.origin, "token:wrong"));
    CHECK(store.checkAndConsumeIfGranted(key.capability, key.origin, key.identity));
    CHECK(!store.checkAndConsumeIfGranted(key.capability, key.origin, key.identity));
    CHECK_EQ(store.sessionGrantCount(7), 1u);
    store.dropSession(8);
    CHECK_EQ(store.sessionGrantCount(7), 1u);
    store.dropSession(7);
    CHECK_EQ(store.sessionGrantCount(7), 0u);
    CHECK(original.grants[0].second.scope == CapabilityGrantScope::OneShot);
}

TEST(consentContinuationRetainsResolvedUndrainedKeysAndRepeatOrder)
{
    CapabilityConsentQueue source;
    REQUIRE(source.recordPending(key.capability, key.origin, key.identity));
    source.resolve(key.capability, key.origin, key.identity);
    REQUIRE(source.recordPending(key.capability, key.origin, key.identity));
    REQUIRE(source.recordPending(Capability::PlaySound, CapabilityOrigin::Local, "resolved"));
    source.resolve(Capability::PlaySound, CapabilityOrigin::Local, "resolved");
    const auto captured = source.continuation(4);
    REQUIRE(captured);
    CHECK(!source.continuation(3));
    auto tables = state();
    tables.pendingConsent = captured->pending;
    tables.newlyPendingConsent = captured->newlyPending;
    const auto encoded = encodeHandoverAuthority(tables, limits);
    REQUIRE(encoded);
    const auto restored = decodeHandoverAuthority(encoded->view(), limits);
    REQUIRE(restored);
    CapabilityConsentQueue target;
    CapabilityConsentQueue::Continuation owner { restored->pendingConsent, restored->newlyPendingConsent };
    REQUIRE(target.restoreContinuation(std::move(owner), 4));
    CHECK(owner.pending.empty() && owner.newlyPending.empty());
    CHECK(!target.recordPending(key.capability, key.origin, key.identity));
    CHECK(target.takeNewlyPending() == source.takeNewlyPending());
    CHECK(!target.isPending(Capability::PlaySound, CapabilityOrigin::Local, "resolved"));
    target.resolve(key.capability, key.origin, key.identity);
    CHECK(target.recordPending(key.capability, key.origin, key.identity));
    CHECK(source.recordPending(Capability::PlaySound, CapabilityOrigin::Local, "resolved"));
}

TEST(consentCandidateRefusesWithoutConsumingEitherOwner)
{
    CapabilityConsentQueue target;
    CapabilityConsentQueue::Continuation duplicate { { key, key }, { key } };
    CHECK(!target.restoreContinuation(std::move(duplicate), 3));
    CHECK_EQ(duplicate.pending.size(), 2u);
    CHECK(!target.isPending(key.capability, key.origin, key.identity));
    CapabilityConsentQueue::Continuation valid { { key }, { key } };
    CHECK(!target.restoreContinuation(std::move(valid), 1));
    REQUIRE(target.recordPending(Capability::PlaySound, CapabilityOrigin::Local, "neighbor"));
    CHECK(!target.restoreContinuation(std::move(valid), 2));
    CHECK_EQ(valid.pending.size(), 1u);
    CHECK(target.isPending(Capability::PlaySound, CapabilityOrigin::Local, "neighbor"));
}

// The layout keeps the count of a pending-question table that no record
// fills. One that names a question comes from a predecessor that was not at
// ground, and is refused as that rather than taken over.
TEST(authorityRefusesARecordNamingAPendingQuestion)
{
    auto source = state();
    source.quicTokens.clear();
    source.alwaysAllowedAgents.clear();
    const auto encoded = encodeHandoverAuthority(source, limits);
    REQUIRE(encoded);
    // After the count: the empty token and agent counts, both request
    // counters, two flags and the digest.
    constexpr std::size_t afterCount = 4 + 4 + 4 + 4 + 1 + 1 + 16;
    auto bytes = bytesOf(*encoded);
    REQUIRE(bytes.size() > afterCount + 4);
    const std::size_t at = bytes.size() - afterCount - 4;
    REQUIRE(std::ranges::all_of(std::span(bytes).subspan(at, 4), [](std::uint8_t byte) { return byte == 0; }));
    bytes[at] = 1;
    redigest(bytes);
    const auto refused = decodeHandoverAuthority(bytes, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == Sync::HandoverBodyError::NotAtGround);
    // A count damaged in transit is the damage it is, never a predecessor
    // that is only not yet at ground and worth waiting for.
    auto damaged = bytesOf(*encoded);
    damaged[at] = 1;
    const auto unreadable = decodeHandoverAuthority(damaged, limits);
    REQUIRE(!unreadable);
    CHECK_MSG(unreadable.error() == Sync::HandoverBodyError::DigestMismatch, "a damaged question count read as a host not yet at ground");
    bytes[at] = 0;
    redigest(bytes);
    CHECK(decodeHandoverAuthority(bytes, limits));
}

TEST(authorityRejectsDuplicateOwnersAndChangedTokenBinding)
{
    auto source = state();
    source.grants.push_back(source.grants[0]);
    CHECK(!encodeHandoverAuthority(source, limits));
    source = state();
    source.pendingConsent.push_back(key);
    CHECK(!encodeHandoverAuthority(source, limits));
    source = state();
    source.quicTokens[0].token[3] = 'x';
    CHECK(!encodeHandoverAuthority(source, limits));
    source = state();
    source.quicTokens.push_back(source.quicTokens[0]);
    CHECK(!encodeHandoverAuthority(source, limits));
    source = state();
    source.quicTokens[0].scope = Protocol::Sync::ScopeSet::fromRaw(128);
    CHECK(!encodeHandoverAuthority(source, limits));
}

TEST(authorityDecoderRefusesTruncationFutureVersionAndNonconfigurableOverride)
{
    const auto encoded = encodeHandoverAuthority(state(), limits);
    REQUIRE(encoded);
    for (std::size_t size = 0; size < encoded->size; ++size)
        CHECK(!decodeHandoverAuthority(encoded->view().first(size), limits));
    const std::array<std::uint8_t, 2> future { 2, 0 };
    const auto newer = decodeHandoverAuthority(future, limits);
    REQUIRE(!newer);
    CHECK(newer.error() == Sync::HandoverBodyError::NewerVersion);
    auto bytes = bytesOf(*encoded);
    bytes[6 + std::to_underlying(CapabilityTier::Benign)] = std::to_underlying(CapabilityDecision::Deny);
    redigest(bytes);
    CHECK(!decodeHandoverAuthority(bytes, limits));
    bytes = bytesOf(*encoded);
    bytes.back() ^= 1;
    CHECK(!decodeHandoverAuthority(bytes, limits));
    bytes = bytesOf(*encoded);
    bytes.push_back(0);
    CHECK(!decodeHandoverAuthority(bytes, limits));
}

TEST(authorityAdmissionHonorsExactAggregateReceiverBounds)
{
    const auto encoded = encodeHandoverAuthority(state(), limits);
    REQUIRE(encoded);
    auto bounded = limits;
    bounded.maxSectionBytes = encoded->size;
    bounded.maxGrants = 2;
    bounded.maxConsentEntries = 4;
    bounded.maxQUICTokens = 1;
    bounded.maxStandingAgents = 2;
    CHECK(decodeHandoverAuthority(encoded->view(), bounded));
    --bounded.maxConsentEntries;
    CHECK(!decodeHandoverAuthority(encoded->view(), bounded));
    bounded = limits;
    bounded.maxStringBytes = 1;
    CHECK(!decodeHandoverAuthority(encoded->view(), bounded));
    bounded = limits;
    bounded.maxQUICTokens = 0;
    CHECK(!decodeHandoverAuthority(encoded->view(), bounded));
}

UBIQUITTY_TEST_MAIN()
