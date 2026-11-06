// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/OSC/PublishedFaces.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"
#include "Platform/Host/HandoverTables.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

using namespace Ubiquitty::Protocol::Sync;
using namespace Ubiquitty::Host;

namespace {

constexpr HandoverAttachmentLimits limits { 1024 * 1024, 64, 256, 256, 65536, 4096, 4096 };
constexpr ID128 predecessor { 71, 93 };
constexpr ID128 successor { 81, 103 };
constexpr ID128 client { 20, 31 };
constexpr ID128 paneID { 91, 43 };

class Secrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 71 }; }

private:
    std::uint64_t m_next = 0;
};

AttachmentHandover state()
{
    Secrets secrets;
    AttachmentTable table(predecessor, measuredResumePolicy(), secrets);
    auto& attachment = table.create(client, ClientKind::Human, { "token:exact-root", true, ClientKind::Human }, 17, 100);
    auto& pane = attachment.paneFor(paneID);
    pane.sentGeneration = 13;
    pane.sentBaseline.dictionaryEpoch = 4;
    pane.sentBaseline.marks = { 20, 7, 9 };
    pane.sentBaseline.columns = 80;
    pane.sentBaseline.rowHashes = { std::vector<std::uint64_t> { 10, 11 }, std::vector<std::uint64_t> { 20, 21 } };
    pane.sentBaseline.blockHashes = { 1, 2, 3, 4, 5, 6, 7 };
    pane.sentBaseline.richTextSerials = { 3, 8 };
    pane.acknowledgedGeneration = 11;
    pane.acknowledgedBaseline = pane.sentBaseline;
    pane.acknowledgedBaseline.rowHashes[1][0] = 32;
    pane.acknowledgedHistory = { 5, 1024, 81 };
    pane.inFlight.push_back({ 12, pane.sentBaseline, { 5, 1025, 82 } });
    pane.inFlight.push_back({ 13, pane.sentBaseline, { 5, 1026, 83 } });
    pane.hasEffectReader = true;
    pane.effectsSent = 15;
    pane.effectsAcknowledged = 10;
    table.noteTransportLost(attachment.attachmentID, 17, 120);
    AttachmentHandover result { table.exportForHandover(200), {}, { { predecessor, 999 } } };
    result.grants.push_back({ attachment.attachmentID,
        Ubiquitty::Protocol::Sync::Scope::Observe | Ubiquitty::Protocol::Sync::Scope::Drive,
        "exact-root", 9000, true, true, true });
    return result;
}

std::vector<std::uint8_t> bytesOf(const HandoverBytes& bytes)
{
    return { bytes.view().begin(), bytes.view().end() };
}

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = digestOf(bytes);
    WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}

std::size_t identityOffset(std::span<const std::uint8_t> bytes, ID128 identity)
{
    std::vector<std::uint8_t> needle;
    WireWriter writer(needle);
    writer.u64(identity.low);
    writer.u64(identity.high);
    const auto found = std::ranges::search(bytes, needle);
    return static_cast<std::size_t>(found.begin() - bytes.begin());
}

} // namespace

TEST(attachmentTableBytesRetainEveryBaselineAndExactGrant)
{
    auto original = state();
    const auto* originalRecord = original.attachments.records[0].get();
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(restored);
    REQUIRE(restored->attachments.records.size() == 1);
    const auto& record = *restored->attachments.records[0];
    CHECK(&record != originalRecord);
    CHECK(record.resumeSecret == originalRecord->resumeSecret);
    CHECK(record.authority == originalRecord->authority);
    CHECK_EQ(record.lostSinceMicroseconds, 120u);
    REQUIRE(record.panes.size() == 1);
    const auto& pane = *record.panes[0];
    CHECK(pane.acknowledgedBaseline.rowHashes == originalRecord->panes[0]->acknowledgedBaseline.rowHashes);
    CHECK(pane.sentBaseline.rowHashes == originalRecord->panes[0]->sentBaseline.rowHashes);
    CHECK(pane.sentBaseline.marks == originalRecord->panes[0]->sentBaseline.marks);
    CHECK(pane.sentBaseline.blockHashes == originalRecord->panes[0]->sentBaseline.blockHashes);
    CHECK(pane.sentBaseline.richTextSerials == originalRecord->panes[0]->sentBaseline.richTextSerials);
    REQUIRE(pane.inFlight.size() == 2);
    CHECK_EQ(pane.inFlight[0].generation, 12u);
    CHECK_EQ(pane.inFlight[1].history.head, 1026u);
    CHECK_EQ(pane.effectsSent, 15u);
    CHECK_EQ(pane.effectsAcknowledged, 10u);
    REQUIRE(restored->grants.size() == 1);
    CHECK(restored->grants[0].scope == original.grants[0].scope);
    CHECK(restored->grants[0].tokenID == "exact-root");
    CHECK_EQ(restored->grants[0].tokenExpiryUnix, 9000u);
    CHECK(restored->grants[0].allowsProof && restored->grants[0].isInteractiveUser);
    REQUIRE(restored->hostIdentities.size() == 1);
    CHECK(restored->hostIdentities[0].issuer == predecessor);
    CHECK_EQ(restored->hostIdentities[0].daemonInstanceID, 999u);
    auto reencoded = encodeHandoverAttachments(*restored, limits);
    REQUIRE(reencoded);
    CHECK(std::ranges::equal(encoded->view(), reencoded->view()));
    CHECK(original.attachments.records[0].get() == originalRecord);
}

TEST(twoDrivingFaceOffersKeepTheSameMeasureAcrossHandover)
{
    auto original = state();
    auto second = state();
    second.attachments.records[0]->attachmentID = { 220, 330 };
    second.attachments.records[0]->resumeSecret = { 221, 331 };
    second.attachments.records[0]->clientInstance = { 222, 332 };
    second.grants[0].attachmentID = second.attachments.records[0]->attachmentID;
    original.attachments.records.push_back(std::move(second.attachments.records[0]));
    original.grants.push_back(std::move(second.grants[0]));

    const auto faceOf = [](std::uint16_t advance) {
        auto face = std::make_shared<Ubiquitty::OSC::TextSizing::ProportionalFaceFacts>();
        face->cellAdvance = 512;
        face->untabledAdvances.fill(advance);
        for (auto& table : face->advances)
            table.fill(advance);
        for (auto& table : face->weightedAdvances)
            table.fill(static_cast<std::uint8_t>((static_cast<std::uint32_t>(advance) * Ubiquitty::OSC::TextSizing::weightedAdvanceFractionsPerCell + face->cellAdvance - 1) / face->cellAdvance));
        return face;
    };
    original.attachments.records[0]->inheritedProportionalFaces = faceOf(640);
    original.attachments.records[1]->inheritedProportionalFaces = faceOf(832);
    constexpr std::array<std::uint32_t, 4> name { 'N', 'a', 'm', 'e' };
    using Measurer = Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer;
    std::array<std::shared_ptr<const Ubiquitty::OSC::TextSizing::ProportionalFaceFacts>, 2> beforeFaces {
        original.attachments.records[0]->inheritedProportionalFaces,
        original.attachments.records[1]->inheritedProportionalFaces,
    };
    Measurer before;
    before.setFaces(beforeFaces, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
    const int expected = before.proportionalRunCells(name, {});
    CHECK(expected > 4);

    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(restored);
    Secrets secrets;
    AttachmentTable successorTable(successor, measuredResumePolicy(), secrets);
    REQUIRE(successorTable.inherit(std::move(restored->attachments), predecessor, 200));
    std::array<const AttachmentRecord*, 2> inherited {};
    REQUIRE(successorTable.inheritedFaceOffers(paneID, inherited) == 2u);
    std::array<std::shared_ptr<const Ubiquitty::OSC::TextSizing::ProportionalFaceFacts>, 2> afterFaces {
        inherited[0]->inheritedProportionalFaces,
        inherited[1]->inheritedProportionalFaces,
    };
    Measurer after;
    after.setFaces(afterFaces, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_EQ(after.proportionalRunCells(name, {}), expected);
}

// A host reads only the attachment section it writes; an earlier version is
// refused rather than read in another layout.
TEST(anEarlierAttachmentSectionVersionIsRefused)
{
    auto encoded = encodeHandoverAttachments(state(), limits);
    REQUIRE(encoded);
    auto earlier = bytesOf(*encoded);
    earlier[0] = 1;
    earlier[1] = 0;
    redigest(earlier);
    const auto refused = decodeHandoverAttachments(earlier, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::Malformed);
}

TEST(decodedAuthorityResumesWithTheOriginalSecretAndGrace)
{
    auto original = state();
    const auto attachmentID = original.attachments.records[0]->attachmentID;
    const auto secret = original.attachments.records[0]->resumeSecret;
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(restored);
    Secrets secrets;
    (void)secrets.nextSecret();
    AttachmentTable table(successor, ResumePolicy { 1000 }, secrets);
    REQUIRE(table.inherit(std::move(restored->attachments), predecessor, 1100));
    CHECK_EQ(table.find(attachmentID)->lostSinceMicroseconds, 120u);
    const ResumeRequest request { attachmentID, secret, {} };
    CHECK(!table.resume(request, { "token:other", true, ClientKind::Human }, 31, 1110));
    CHECK(table.find(attachmentID)->resumeSecret == secret);
    auto resumed = table.resume(request, { "token:exact-root", true, ClientKind::Human }, 32, 1110);
    REQUIRE(resumed);
    CHECK((*resumed)->issuer == successor);
    CHECK((*resumed)->resumeSecret != secret);
    CHECK_EQ((*resumed)->panes[0]->acknowledgedGeneration.value_or(0), 11u);
    CHECK(!table.resume(request, { "token:exact-root", true, ClientKind::Human }, 33, 1111));
}

TEST(attachmentCodecPreservesTransitiveIssuerAndUnboundPaneOrder)
{
    auto original = state();
    const ID128 older { 201, 202 };
    original.attachments.inheritedInstances.push_back(older);
    original.hostIdentities.insert(original.hostIdentities.begin(), { older, 501 });
    original.attachments.records[0]->issuer = older;
    auto& unbound = original.attachments.records[0]->paneFor({ 51, 53 });
    unbound.isHeld = false;
    original.attachments.revision = 456;
    original.attachments.notifyRevision = 123;
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto decoded = decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(decoded);
    CHECK(decoded->attachments.records[0]->issuer == older);
    CHECK_EQ(decoded->attachments.revision, 456u);
    CHECK_EQ(decoded->attachments.notifyRevision, 123u);
    CHECK(decoded->attachments.inheritedInstances == original.attachments.inheritedInstances);
    REQUIRE(decoded->attachments.records[0]->panes.size() == 2);
    CHECK(decoded->attachments.records[0]->panes[0]->paneID == paneID);
    CHECK(!decoded->attachments.records[0]->panes[1]->isHeld);
    CHECK(decoded->hostIdentities[0].issuer == older);
    CHECK_EQ(decoded->hostIdentities[1].daemonInstanceID, 999u);
}

TEST(codecNeverConvertsPhysicalOwnersOrConsumedSecretsToPortableAuthority)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto original = state();
        auto& record = *original.attachments.records[0];
        switch (mode) {
        case 0:
            record.state = AttachmentState::Live;
            break;
        case 1:
            record.state = AttachmentState::Detached;
            break;
        case 2:
            record.servingConnection = 71;
            break;
        case 3:
            record.consumedSecret = ID128 { 1, 2 };
            break;
        case 4:
            record.handshakeConnection = 71;
            break;
        case 5:
            record.proofConnection = 71;
            break;
        }
        auto refused = encodeHandoverAttachments(original, limits);
        CHECK(!refused);
        CHECK(original.attachments.records[0].get() == &record);
    }
}

TEST(attachmentSectionRejectsEveryTruncationCorruptionAndTrailingBytes)
{
    const auto original = state();
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    for (std::size_t size = 0; size < encoded->size; ++size)
        CHECK(!decodeHandoverAttachments(encoded->view().first(size), limits));
    auto corrupted = bytesOf(*encoded);
    corrupted[identityOffset(corrupted, client)] ^= 1;
    auto refused = decodeHandoverAttachments(corrupted, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::DigestMismatch);
    corrupted = bytesOf(*encoded);
    corrupted.push_back(0);
    CHECK(!decodeHandoverAttachments(corrupted, limits));
    std::array<std::uint8_t, 2> future { encoded->view()[0], encoded->view()[1] };
    REQUIRE(future[0] != 0xff);
    ++future[0];
    refused = decodeHandoverAttachments(future, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::NewerVersion);
}

TEST(attachmentAdmissionChargesAggregateBaselinesAndStrings)
{
    const auto original = state();
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    for (unsigned mode = 0; mode < 7; ++mode) {
        auto smaller = limits;
        switch (mode) {
        case 0:
            smaller.maxSectionBytes = encoded->size - 1;
            break;
        case 1:
            smaller.maxAttachments = 0;
            break;
        case 2:
            smaller.maxPaneRecords = 0;
            break;
        case 3:
            smaller.maxInFlightGenerations = 1;
            break;
        case 4:
            smaller.maxRowHashes = 15;
            break; // Four baselines, four hashes each.
        case 5:
            smaller.maxRichTextSerials = 7;
            break;
        case 6:
            smaller.maxStringBytes = 25;
            break; // Sixteen principal + ten token bytes.
        }
        auto refused = decodeHandoverAttachments(encoded->view(), smaller);
        REQUIRE(!refused);
        CHECK(refused.error() == HandoverBodyError::OverCapacity);
        CHECK(!encodeHandoverAttachments(original, smaller));
    }
    auto exact = limits;
    exact.maxSectionBytes = encoded->size;
    exact.maxAttachments = 1;
    exact.maxPaneRecords = 1;
    exact.maxInFlightGenerations = 2;
    exact.maxRowHashes = 16;
    exact.maxRichTextSerials = 8;
    exact.maxStringBytes = 26;
    CHECK(decodeHandoverAttachments(encoded->view(), exact));
}

TEST(attachmentCodecRefusesUnknownAuthorityAndMalformedBaselines)
{
    for (unsigned mode = 0; mode < 12; ++mode) {
        auto original = state();
        auto& record = *original.attachments.records[0];
        auto& pane = *record.panes[0];
        switch (mode) {
        case 0:
            original.grants.clear();
            break;
        case 1:
            original.grants[0].tokenID = "other-root";
            break;
        case 2:
            original.grants[0].scope = {};
            break;
        case 3:
            original.grants[0].isInteractiveUser = false;
            break;
        case 4:
            original.grants[0].hasBootstrapToken = false;
            break;
        case 5:
            original.hostIdentities[0].issuer = successor;
            break;
        case 6:
            original.hostIdentities[0].daemonInstanceID = 0;
            break;
        case 7:
            record.authority.verifiedKind = static_cast<ClientKind>(99);
            break;
        case 8:
            pane.effectsAcknowledged = pane.effectsSent + 1;
            break;
        case 9:
            pane.inFlight[1].generation = pane.inFlight[0].generation;
            break;
        case 10:
            pane.sentBaseline.richTextSerials = { 8, 3 };
            break;
        case 11:
            pane.sentBaseline.columns = -1;
            break;
        }
        CHECK(!encodeHandoverAttachments(original, limits));
    }
}

TEST(rechecksummedMalformedIdentityAndBooleanAreStillRefused)
{
    const auto original = state();
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto bytes = bytesOf(*encoded);
    const auto clientOffset = identityOffset(bytes, client);
    REQUIRE(clientOffset >= 3 && clientOffset < bytes.size());
    bytes[clientOffset - 3] = 2; // mayDrive must be canonical Boolean.
    redigest(bytes);
    CHECK(!decodeHandoverAttachments(bytes, limits));
    bytes = bytesOf(*encoded);
    const auto issuerOffset = identityOffset(bytes, original.attachments.records[0]->attachmentID) + 16;
    REQUIRE(issuerOffset + 16 < bytes.size());
    std::fill_n(bytes.begin() + issuerOffset, 16, 0);
    redigest(bytes);
    CHECK(!decodeHandoverAttachments(bytes, limits));
    bytes = bytesOf(*encoded);
    // The fixed header's inherited count is independently bounded before allocation.
    std::fill_n(bytes.begin() + 38, 4, 0xFF);
    redigest(bytes);
    CHECK(!decodeHandoverAttachments(bytes, limits));
}

TEST(serializedSecretProofIsBoundToItsOriginalIssuer)
{
    auto original = state();
    const auto id = original.attachments.records[0]->attachmentID;
    const auto secret = original.attachments.records[0]->resumeSecret;
    auto encoded = encodeHandoverAttachments(original, limits);
    REQUIRE(encoded);
    auto decoded = decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(decoded);
    Secrets secrets;
    AttachmentTable table(successor, ResumePolicy { 1000 }, secrets);
    REQUIRE(table.inherit(std::move(decoded->attachments), predecessor, 200));
    const std::array<std::uint8_t, 32> exporter { 11, 12, 13 };
    const ResumeProof wrong { id, resumeProofMAC(secret, exporter, id, successor) };
    CHECK(!table.admitProof(wrong, exporter, 30, 300));
    CHECK(!table.find(id)->proofConnection);
    const ResumeProof right { id, resumeProofMAC(secret, exporter, id, predecessor) };
    CHECK(table.admitProof(right, exporter, 30, 300));
    table.abandonProof(30);
    table.expire(1120);
    CHECK(!table.find(id));
}

UBIQUITTY_TEST_MAIN()
