// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The `Notify` kinds (Docs/sync.md §7.3): each kind's record round-trips
// through its own encoder and decoder, a fixture written byte by byte reads
// the same as the encoder's output, and a record that is truncated, extended
// or out of bounds refuses whole.

#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr ID128 attachmentID { 0xa1, 0xb2 };
constexpr std::array paneIDs { ID128 { 1, 3 }, ID128 { 2, 3 }, ID128 { 3, 3 } };

/// The record of a whole body, after its kind, which must be `kind`.
std::vector<std::uint8_t> recordOf(const std::vector<std::uint8_t>& body, NotifyKind kind)
{
    const auto head = decodeNotifyHead(body);
    CHECK(head && head->kind == std::to_underlying(kind));
    return head ? std::vector<std::uint8_t>(head->record.begin(), head->record.end()) : std::vector<std::uint8_t> {};
}

AttachmentsList domainList()
{
    AttachmentPresence presence { .clientID = 7, .processID = 42, .process = "/verified/tool", .claimedLabel = "a claimed label", .grantIdentity = "token:verified-identity", .isQUIC = true, .sharedPanes = {} };
    for (std::size_t index = 0; index < paneIDs.size(); ++index)
        presence.sharedPanes.push_back({ paneIDs[index], static_cast<PresenceGrant>(index) });
    AttachmentEntry entry { .attachmentID = attachmentID, .role = AttachmentRole::Driver, .clientKind = ClientKind::Human, .state = AttachmentState::Live, .isGeometryOwner = true, .isPrimary = true, .panes = { paneIDs.begin(), paneIDs.end() }, .isSelf = true, .principal = "owner-only-principal", .presence = std::move(presence) };
    return { .revision = UINT64_MAX, .attachments = { std::move(entry) } };
}

/// The record `domainList` encodes to, written field by field rather than
/// through the encoder.
std::vector<std::uint8_t> handWrittenRecord()
{
    std::vector<std::uint8_t> record;
    WireWriter writer(record);
    const auto id = [&](ID128 value) {
        writer.u64(value.low);
        writer.u64(value.high);
    };
    writer.u64(UINT64_MAX);
    writer.varint(1);
    id(attachmentID);
    for (const std::uint8_t byte : { 1, 0, 0, 1, 1, 1 })
        writer.u8(byte);
    writer.varint(paneIDs.size());
    for (const auto pane : paneIDs)
        id(pane);
    writer.u8(1);
    writer.utf8("owner-only-principal");
    writer.u8(1);
    writer.u32(7);
    writer.u32(42);
    writer.utf8("/verified/tool");
    writer.utf8("a claimed label");
    writer.utf8("token:verified-identity");
    writer.u8(1);
    writer.varint(paneIDs.size());
    for (std::size_t index = 0; index < paneIDs.size(); ++index) {
        id(paneIDs[index]);
        writer.u8(static_cast<std::uint8_t>(index));
    }
    return record;
}

/// A copy of a record with one byte replaced.
std::vector<std::uint8_t> withByte(std::vector<std::uint8_t> record, std::size_t offset, std::uint8_t value)
{
    record.at(offset) = value;
    return record;
}

/// Whether no proper prefix of the record, and no extension of it, reads.
template<typename Decode>
bool refusesEveryCutAndExtension(const std::vector<std::uint8_t>& record, Decode decode)
{
    for (std::size_t cut = 0; cut < record.size(); ++cut) {
        if (decode(std::span(record).first(cut)))
            return false;
    }
    auto extended = record;
    extended.push_back(0);
    return !decode(std::span<const std::uint8_t>(extended));
}

// The role byte follows the revision, the entry count and the ID.
constexpr std::size_t roleOffset = 8 + 1 + 16;

} // namespace

TEST(everyNotifyKindHasOneWireNumberAndTheRestAreUnknown)
{
    const std::array known { NotifyKind::Attachments, NotifyKind::Events, NotifyKind::ConsentStatus, NotifyKind::ConsentQuestion,
        NotifyKind::StructureConsent, NotifyKind::ConsentResolved, NotifyKind::PasteReady };
    for (std::uint64_t value = 0; value < 300; ++value) {
        const auto kind = notifyKindOf(value);
        const bool isKnown = std::ranges::any_of(known, [&](NotifyKind candidate) { return std::to_underlying(candidate) == value; });
        CHECK(kind.has_value() == isKnown);
        if (kind)
            CHECK_EQ(std::to_underlying(*kind), value);
    }
    CHECK(!notifyKindOf(UINT64_MAX));
    CHECK(directionOf(FrameKind::Notify) == Direction::HostToClient);
}

TEST(aNotifyHeadIsACanonicalVarintAndTheRecordIsTheRest)
{
    const std::vector<std::uint8_t> body { 0x80, 0x01, 0xAA, 0xBB };
    const auto head = decodeNotifyHead(body);
    REQUIRE(head);
    CHECK_EQ(head->kind, 128u);
    CHECK(head->record.size() == 2 && head->record[0] == 0xAA);
    CHECK(!decodeNotifyHead({}));
    CHECK(!decodeNotifyHead(std::vector<std::uint8_t> { 0x80 }));
    CHECK(!decodeNotifyHead(std::vector<std::uint8_t> { 0x83, 0x00 }));
    std::vector<std::uint8_t> encoded;
    encode(encoded, Notify { .kind = NotifyKind::PasteReady, .record = { 1, 2 } });
    CHECK((encoded == std::vector<std::uint8_t> { 7, 1, 2 }));
}

TEST(attachmentsRoundTripWithDistinctPerPaneGrants)
{
    const auto expected = domainList();
    const auto body = attachmentsNotify(expected);
    REQUIRE(body);
    const auto record = recordOf(*body, NotifyKind::Attachments);
    CHECK(record == handWrittenRecord());
    const auto decoded = decodeAttachments(record);
    REQUIRE(decoded);
    CHECK_EQ(decoded->revision, UINT64_MAX);
    CHECK(decoded->attachments == expected.attachments);
    const auto& presence = *decoded->attachments[0].presence;
    CHECK(presence.sharedPanes[0].grant == PresenceGrant::None);
    CHECK(presence.sharedPanes[1].grant == PresenceGrant::Session);
    CHECK(presence.sharedPanes[2].grant == PresenceGrant::Always);

    auto withLost = expected;
    withLost.attachments.push_back({ .attachmentID = { 9, 10 }, .role = AttachmentRole::Observer, .clientKind = ClientKind::Agent, .state = AttachmentState::Lost, .isGeometryOwner = false, .isPrimary = false, .panes = { paneIDs[1] }, .isSelf = false, .principal = std::nullopt, .presence = std::nullopt });
    const auto lost = attachmentsNotify(withLost);
    REQUIRE(lost);
    const auto lostDecoded = decodeAttachments(recordOf(*lost, NotifyKind::Attachments));
    REQUIRE(lostDecoded);
    CHECK(lostDecoded->attachments == withLost.attachments);
}

TEST(attachmentsRefuseUnknownEnumsLooseBooleansAndZeroIDs)
{
    const auto record = handWrittenRecord();
    REQUIRE(decodeAttachments(record));
    CHECK(!decodeAttachments(withByte(record, roleOffset, 2)));
    CHECK(!decodeAttachments(withByte(record, roleOffset + 1, 3)));
    CHECK(!decodeAttachments(withByte(record, roleOffset + 2, 2)));
    for (std::size_t flag = 3; flag < 6; ++flag)
        CHECK(!decodeAttachments(withByte(record, roleOffset + flag, 2)));
    auto zeroID = record;
    std::fill(zeroID.begin() + 9, zeroID.begin() + 9 + 16, std::uint8_t { 0 });
    CHECK(!decodeAttachments(zeroID));
    auto zeroRevision = record;
    std::fill(zeroRevision.begin(), zeroRevision.begin() + 8, std::uint8_t { 0 });
    CHECK(!decodeAttachments(zeroRevision));
    auto invalid = domainList();
    invalid.attachments[0].role = static_cast<AttachmentRole>(255);
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].clientKind = static_cast<ClientKind>(255);
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].state = static_cast<AttachmentState>(255);
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.revision = 0;
    CHECK(!attachmentsNotify(invalid));
}

TEST(attachmentsPresenceIsLiveAndNamesEveryBasePaneOnce)
{
    auto invalid = domainList();
    invalid.attachments[0].presence->sharedPanes[2].grant = static_cast<PresenceGrant>(255);
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].state = AttachmentState::Lost;
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].presence->sharedPanes.pop_back();
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].presence->sharedPanes[2].paneID = paneIDs[0];
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].presence->sharedPanes[2].paneID = { 99, 99 };
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].presence->clientID = 0;
    CHECK(!attachmentsNotify(invalid));
    invalid = domainList();
    invalid.attachments[0].presence->processID = 0;
    CHECK(!attachmentsNotify(invalid));
    invalid.attachments[0].presence->process.clear();
    CHECK(attachmentsNotify(invalid).has_value());

    // The decoder holds the same rules against a hostile host: the last grant
    // byte past Always, and a lost row carrying presence.
    const auto record = handWrittenRecord();
    CHECK(!decodeAttachments(withByte(record, record.size() - 1, 3)));
    CHECK(!decodeAttachments(withByte(record, roleOffset + 2, 1)));
}

TEST(attachmentsBoundVerifiedFactsAndKeepClaimedLabelsSeparate)
{
    auto list = domainList();
    list.attachments[0].presence->claimedLabel = std::string(256, 'm');
    CHECK(attachmentsNotify(list).has_value());
    list.attachments[0].presence->claimedLabel = std::string(257, 'm');
    CHECK(!attachmentsNotify(list));
    for (const std::string_view key : { "process", "claimedLabel", "grantIdentity", "principal" }) {
        for (const std::string& malformed : { std::string("\xc0\xaf", 2), std::string("a\0b", 3) }) {
            auto invalid = domainList();
            if (key == "process")
                invalid.attachments[0].presence->process = malformed;
            if (key == "claimedLabel")
                invalid.attachments[0].presence->claimedLabel = malformed;
            if (key == "grantIdentity")
                invalid.attachments[0].presence->grantIdentity = malformed;
            if (key == "principal")
                invalid.attachments[0].principal = malformed;
            CHECK(!attachmentsNotify(invalid));
        }
    }
}

TEST(attachmentsAdmitExactlyOneFrameAndRefuseTruncationOrTrailingBytes)
{
    auto list = domainList();
    auto& row = list.attachments[0];
    row.principal = "";
    row.presence->process = "";
    row.presence->processID = 0;
    row.presence->claimedLabel = "";
    row.presence->grantIdentity = "";
    const auto empty = attachmentsNotify(list);
    REQUIRE(empty && empty->size() < maxFrameBodyBytes);
    // The principal's length varint grows from one byte to three.
    const auto room = maxFrameBodyBytes - empty->size() - 2;
    row.principal = std::string(room, 'x');
    const auto maximum = attachmentsNotify(list);
    REQUIRE(maximum);
    CHECK_EQ(maximum->size(), maxFrameBodyBytes);
    const auto decoded = decodeAttachments(recordOf(*maximum, NotifyKind::Attachments));
    REQUIRE(decoded);
    CHECK(decoded->attachments == list.attachments);
    row.principal->push_back('x');
    CHECK(!attachmentsNotify(list));
    CHECK(!attachmentsNotifySize(list));
    CHECK(refusesEveryCutAndExtension(handWrittenRecord(), [](auto record) { return decodeAttachments(record).has_value(); }));
}

TEST(attachmentsRefuseDuplicateRowsPanesAndAmbiguousSelf)
{
    auto list = domainList();
    list.attachments[0].presence.reset();
    list.attachments[0].panes.clear();
    CHECK(attachmentsNotify(list).has_value());
    list.attachments[0].isSelf = false;
    CHECK(!attachmentsNotify(list));
    list = domainList();
    list.attachments.push_back(list.attachments[0]);
    CHECK(!attachmentsNotify(list));
    list.attachments[1].attachmentID = { 88, 99 };
    CHECK(!attachmentsNotify(list));
    list.attachments[1].isSelf = false;
    CHECK(attachmentsNotify(list).has_value());
    list.attachments[1].panes[1] = list.attachments[1].panes[0];
    CHECK(!attachmentsNotify(list));
    list = domainList();
    list.attachments[0].presence.reset();
    list.attachments[0].panes.clear();
    for (std::size_t index = 0; index < maxPanesPerConnection; ++index)
        list.attachments[0].panes.push_back({ index + 1, 1 });
    CHECK(attachmentsNotify(list).has_value());
    list.attachments[0].panes.push_back({ maxPanesPerConnection + 1, 1 });
    CHECK(!attachmentsNotify(list));
    list.attachments.resize(1'025);
    CHECK(!attachmentsNotify(list));
}

TEST(theAttachmentsSizeWalkMatchesEveryEncodedByte)
{
    auto list = domainList();
    const auto size = attachmentsNotifySize(list);
    const auto body = attachmentsNotify(list);
    REQUIRE(size && body);
    CHECK(*size == body->size());
    list.attachments[0].presence->process.assign(40'000, 'p');
    list.attachments[0].presence->grantIdentity.assign(40'000, 'g');
    CHECK(!attachmentsNotifySize(list));
    CHECK(!attachmentsNotify(list));
    list.attachments[0].presence.reset();
    const auto baseSize = attachmentsNotifySize(list);
    const auto baseBody = attachmentsNotify(list);
    REQUIRE(baseSize && baseBody);
    CHECK(*baseSize == baseBody->size());
}

TEST(theOwnConsentStatusIsTenBytesAndRefusesAnyOtherShape)
{
    for (const auto phase : { ConsentStatusPhase::Pending, ConsentStatusPhase::Approved, ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        const ConsentStatus status { .question = { .requestID = 0x01020304, .sessionID = 9 }, .phase = phase };
        const auto body = consentStatusNotify(status);
        REQUIRE(body);
        CHECK_EQ(body->size(), maxConsentStatusBytes);
        CHECK(decodeConsentStatus(recordOf(*body, NotifyKind::ConsentStatus)) == status);
    }
    CHECK(!consentStatusNotify({ .question = { .requestID = 0, .sessionID = 9 }, .phase = ConsentStatusPhase::Pending }));
    CHECK(!consentStatusNotify({ .question = { .requestID = 1, .sessionID = 0 }, .phase = ConsentStatusPhase::Pending }));
    CHECK(!consentStatusNotify({ .question = { .requestID = 1, .sessionID = 1 }, .phase = static_cast<ConsentStatusPhase>(4) }));
    const auto record = recordOf(*consentStatusNotify({ .question = { .requestID = 1, .sessionID = 1 }, .phase = ConsentStatusPhase::Denied }), NotifyKind::ConsentStatus);
    CHECK(!decodeConsentStatus(withByte(record, 8, 4)));
    CHECK(!decodeConsentStatus(withByte(withByte(record, 0, 0), 1, 0)));
    CHECK(refusesEveryCutAndExtension(record, [](auto bytes) { return decodeConsentStatus(bytes).has_value(); }));
}

TEST(aConsentQuestionRoundTripsEveryKindAndRefusesStructure)
{
    ConsentQuestion attach { .requestID = 3, .sessionID = 4, .peerProcessID = 77, .peerProcess = "/usr/bin/agent", .agentLabel = "an agent", .action = std::to_underlying(ConsentKind::Attach) };
    ConsentQuestion terminate = attach;
    terminate.action = std::to_underlying(ConsentKind::Terminate);
    ConsentQuestion capability = attach;
    capability.action = std::to_underlying(ConsentKind::Capability);
    capability.capabilityCode = 2;
    capability.capabilityOrigin = 1;
    capability.isRecipientsOwnRequest = true;
    for (const auto& question : { attach, terminate, capability }) {
        const auto body = consentQuestionNotify(question);
        REQUIRE(body);
        const auto record = recordOf(*body, NotifyKind::ConsentQuestion);
        const auto decoded = decodeConsentQuestion(record);
        REQUIRE(decoded);
        CHECK(decoded->requestID == question.requestID && decoded->sessionID == question.sessionID);
        CHECK(decoded->peerProcessID == question.peerProcessID && decoded->peerProcess == question.peerProcess);
        CHECK(decoded->agentLabel == question.agentLabel && decoded->action == question.action);
        CHECK(decoded->capabilityCode == question.capabilityCode && decoded->capabilityOrigin == question.capabilityOrigin);
        CHECK(decoded->isRecipientsOwnRequest == question.isRecipientsOwnRequest);
        CHECK(refusesEveryCutAndExtension(record, [](auto bytes) { return decodeConsentQuestion(bytes).has_value(); }));
    }
    auto structure = attach;
    structure.action = std::to_underlying(ConsentKind::Structure);
    CHECK(!consentQuestionNotify(structure));
    const auto record = recordOf(*consentQuestionNotify(attach), NotifyKind::ConsentQuestion);
    CHECK(!decodeConsentQuestion(withByte(record, 0, std::to_underlying(ConsentKind::Structure))));
    auto unverified = attach;
    unverified.peerProcessID = 0;
    CHECK(!consentQuestionNotify(unverified));
    auto withNul = attach;
    withNul.agentLabel = std::string("a\0b", 3);
    CHECK(!consentQuestionNotify(withNul));
    auto tooLong = attach;
    tooLong.agentLabel = std::string(257, 'l');
    CHECK(!consentQuestionNotify(tooLong));
    auto capabilityless = attach;
    capabilityless.capabilityCode = 1;
    CHECK(!consentQuestionNotify(capabilityless));
}

TEST(aStructureConsentCarriesItsOperationAndRefusesAnUnknownMethod)
{
    const StructureConsent question { .requestID = 5, .sessionID = 6, .workspaceID = 7, .revision = UINT64_MAX, .workspaceName = "work", .method = "label.set", .parameters = R"({"object":"pane:1","label":"logs"})", .agentLabel = "an agent", .peerProcess = "/usr/bin/agent", .peerProcessID = 9 };
    const auto body = structureConsentNotify(question);
    REQUIRE(body);
    const auto record = recordOf(*body, NotifyKind::StructureConsent);
    CHECK(decodeStructureConsent(record) == question);
    CHECK(refusesEveryCutAndExtension(record, [](auto bytes) { return decodeStructureConsent(bytes).has_value(); }));
    auto unknown = question;
    unknown.method = "pane.write";
    CHECK(!structureConsentNotify(unknown));
    auto unparsed = question;
    unparsed.parameters = "[]";
    CHECK(!structureConsentNotify(unparsed));
    auto zero = question;
    zero.revision = 0;
    CHECK(!structureConsentNotify(zero));
}

TEST(aResolvedQuestionNamesOnlyItsKindAndIdentity)
{
    for (const auto kind : { ConsentKind::Attach, ConsentKind::Terminate, ConsentKind::Capability, ConsentKind::Structure }) {
        const ConsentResolution resolution { .kind = kind, .requestID = 11, .sessionID = 12 };
        const auto body = consentResolvedNotify(resolution);
        REQUIRE(body);
        CHECK_EQ(body->size(), maxConsentResolutionBytes);
        const auto record = recordOf(*body, NotifyKind::ConsentResolved);
        CHECK(decodeConsentResolved(record) == resolution);
        CHECK(refusesEveryCutAndExtension(record, [](auto bytes) { return decodeConsentResolved(bytes).has_value(); }));
    }
    CHECK(!consentResolvedNotify({ .kind = ConsentKind::Attach, .requestID = 0, .sessionID = 1 }));
    CHECK(!consentResolvedNotify({ .kind = static_cast<ConsentKind>(4), .requestID = 1, .sessionID = 1 }));
    CHECK(!decodeConsentResolved(std::vector<std::uint8_t> { 4, 1, 0, 0, 0, 1, 0, 0, 0 }));
}

TEST(pasteCreditAndEventBatchesRoundTrip)
{
    using namespace Ubiquitty::Protocol::Control;
    const PasteReady ready { "paste.7", 7, 0x01020304 };
    const auto credit = pasteReadyNotify(ready);
    REQUIRE(credit);
    const auto creditRecord = recordOf(*credit, NotifyKind::PasteReady);
    CHECK(decodePasteReady(creditRecord) == ready);
    CHECK(refusesEveryCutAndExtension(creditRecord, [](auto bytes) { return decodePasteReady(bytes).has_value(); }));
    CHECK(!pasteReadyNotify({ "", 7, 1 }));
    CHECK(!pasteReadyNotify({ "paste.7", 0, 1 }));
    CHECK(!pasteReadyNotify({ "paste.7", 7, 0 }));

    std::vector<Ubiquitty::Session::Event> events(2);
    events[0].cursor = 4;
    events[0].kind = Ubiquitty::Session::EventKind::Bell;
    events[0].sessionID = 2;
    events[1].cursor = 9;
    events[1].kind = Ubiquitty::Session::EventKind::CommandFinished;
    events[1].exitStatus = -3;
    events[1].text = "make";
    const auto batch = eventsNotify(events);
    REQUIRE(batch);
    const auto batchRecord = recordOf(*batch, NotifyKind::Events);
    const auto decoded = decodeEvents(batchRecord);
    REQUIRE(decoded && decoded->size() == 2);
    CHECK((*decoded)[1].exitStatus == -3 && (*decoded)[1].text == "make" && (*decoded)[0].sessionID == 2);
    CHECK(refusesEveryCutAndExtension(batchRecord, [](auto bytes) { return decodeEvents(bytes).has_value(); }));
    std::swap(events[0], events[1]);
    CHECK(!eventsNotify(events));
    CHECK(!decodeEvents(std::vector<std::uint8_t> { 0 }));
}

UBIQUITTY_TEST_MAIN()
