// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Sync/Wire.h"
#include "PAL/Capability.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

ConsentQuestion question(ConsentKind kind = ConsentKind::Attach)
{
    ConsentQuestion value { .requestID = UINT32_MAX, .sessionID = UINT32_MAX, .peerProcessID = 23, .peerProcess = "/verified/process", .agentLabel = "claimed label", .action = std::to_underlying(kind) };
    if (kind == ConsentKind::Capability) {
        value.capabilityCode = std::to_underlying(Capability::ClipboardWrite);
        value.capabilityOrigin = std::to_underlying(CapabilityOrigin::Remote);
        value.isRecipientsOwnRequest = true;
    }
    return value;
}

/// The record after a whole body's kind, which must be `kind`.
std::vector<std::uint8_t> recordOf(const std::optional<std::vector<std::uint8_t>>& body, NotifyKind kind)
{
    const auto head = body ? decodeNotifyHead(*body) : std::nullopt;
    CHECK(head && head->kind == std::to_underlying(kind));
    return head ? std::vector<std::uint8_t>(head->record.begin(), head->record.end()) : std::vector<std::uint8_t> {};
}

/// A question record written field by field, so one field can be wrong.
struct QuestionRecord {
    std::uint8_t kind = std::to_underlying(ConsentKind::Attach);
    std::uint32_t request = 1;
    std::uint32_t session = 2;
    std::uint32_t processID = 23;
    std::string process = "/verified/process";
    std::string label = "claimed label";
    bool hasCapabilityTail = false;
    std::uint16_t capabilityCode = 0;
    std::uint8_t capabilityOrigin = 0;
    std::uint8_t isOwn = 0;

    std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> record;
        WireWriter writer(record);
        writer.u8(kind);
        writer.u32(request);
        writer.u32(session);
        writer.u32(processID);
        writer.utf8(process);
        writer.utf8(label);
        if (hasCapabilityTail) {
            writer.u16(capabilityCode);
            writer.u8(capabilityOrigin);
            writer.u8(isOwn);
        }
        return record;
    }
};

QuestionRecord capabilityRecord()
{
    QuestionRecord record;
    record.kind = std::to_underlying(ConsentKind::Capability);
    record.hasCapabilityTail = true;
    record.capabilityCode = std::to_underlying(Capability::ClipboardWrite);
    record.capabilityOrigin = std::to_underlying(CapabilityOrigin::Remote);
    record.isOwn = 1;
    return record;
}

} // namespace

TEST(humanConsentQuestionsRetainTheirExactKindAndSeparateVerifiedFactsFromClaims)
{
    for (const auto kind : { ConsentKind::Attach, ConsentKind::Terminate, ConsentKind::Capability }) {
        const auto expected = question(kind);
        const auto actual = decodeConsentQuestion(recordOf(consentQuestionNotify(expected), NotifyKind::ConsentQuestion));
        REQUIRE(actual);
        CHECK(actual->requestID == expected.requestID && actual->sessionID == expected.sessionID);
        CHECK(actual->peerProcessID == expected.peerProcessID && actual->peerProcess == expected.peerProcess);
        CHECK(actual->agentLabel == expected.agentLabel && actual->action == expected.action);
        CHECK(actual->capabilityCode == expected.capabilityCode && actual->capabilityOrigin == expected.capabilityOrigin);
        CHECK(actual->isRecipientsOwnRequest == expected.isRecipientsOwnRequest);
    }
    auto unknown = question();
    unknown.peerProcessID = 0;
    unknown.peerProcess.clear();
    CHECK(decodeConsentQuestion(recordOf(consentQuestionNotify(unknown), NotifyKind::ConsentQuestion))->peerProcess.empty());
    unknown.peerProcess = "claimed-process-is-not-verified";
    CHECK(!consentQuestionNotify(unknown));
    CHECK(!consentQuestionNotify(question(ConsentKind::Structure)));
    for (const auto kind : { ConsentKind::Attach, ConsentKind::Terminate, ConsentKind::Capability, ConsentKind::Structure }) {
        const ConsentResolution expected { kind, UINT32_MAX, UINT32_MAX };
        const auto body = consentResolvedNotify(expected);
        REQUIRE(body);
        CHECK(body->size() == maxConsentResolutionBytes);
        CHECK(decodeConsentResolved(recordOf(body, NotifyKind::ConsentResolved)) == expected);
    }
}

TEST(humanConsentQuestionsRefuseMixedKindsAndUnknownCapabilityTags)
{
    REQUIRE(decodeConsentQuestion(QuestionRecord {}.bytes()));
    REQUIRE(decodeConsentQuestion(capabilityRecord().bytes()));
    auto record = QuestionRecord {};
    record.kind = 4;
    CHECK(!decodeConsentQuestion(record.bytes()));
    record.kind = std::to_underlying(ConsentKind::Structure);
    CHECK(!decodeConsentQuestion(record.bytes()));
    record = QuestionRecord {};
    record.processID = 0;
    CHECK(!decodeConsentQuestion(record.bytes()));
    record = QuestionRecord {};
    record.hasCapabilityTail = true;
    CHECK(!decodeConsentQuestion(record.bytes()));
    auto capability = capabilityRecord();
    capability.capabilityCode = 65535;
    CHECK(!decodeConsentQuestion(capability.bytes()));
    capability = capabilityRecord();
    capability.capabilityOrigin = std::to_underlying(CapabilityOrigin::Remote) + 1;
    CHECK(!decodeConsentQuestion(capability.bytes()));
    capability = capabilityRecord();
    capability.isOwn = 2;
    CHECK(!decodeConsentQuestion(capability.bytes()));
    capability = capabilityRecord();
    capability.hasCapabilityTail = false;
    CHECK(!decodeConsentQuestion(capability.bytes()));
    auto invalid = question();
    invalid.capabilityCode = 1;
    CHECK(!consentQuestionNotify(invalid));
    CHECK(!decodeConsentResolved(QuestionRecord {}.bytes()));
}

TEST(humanConsentQuestionIdentitiesAndTextAreStrictBeforePresentation)
{
    for (const auto field : { 0, 1 }) {
        auto record = QuestionRecord {};
        (field ? record.session : record.request) = 0;
        CHECK(!decodeConsentQuestion(record.bytes()));
    }
    auto record = QuestionRecord {};
    record.label = std::string("claimed\0label", 13);
    CHECK(!decodeConsentQuestion(record.bytes()));
    record = QuestionRecord {};
    record.label.assign("\xc0\xaf", 2);
    CHECK(!decodeConsentQuestion(record.bytes()));
    record = QuestionRecord {};
    record.label.assign(257, 'a');
    CHECK(!decodeConsentQuestion(record.bytes()));
    const auto exact = QuestionRecord {}.bytes();
    for (std::size_t cut = 0; cut < exact.size(); ++cut)
        CHECK(!decodeConsentQuestion(std::span(exact).first(cut)));
    auto extended = exact;
    extended.push_back(0);
    CHECK(!decodeConsentQuestion(extended));
    auto invalid = question();
    invalid.agentLabel.assign(257, 'a');
    CHECK(!consentQuestionNotify(invalid));
    invalid = question();
    invalid.peerProcess.assign(maxConsentIdentityBytes + 1, 'a');
    CHECK(!consentQuestionNotify(invalid));
    invalid = question();
    invalid.agentLabel.assign("\xc0\xaf", 2);
    CHECK(!consentQuestionNotify(invalid));
}

TEST(humanConsentQuestionsAdmitAWholeFrameAndNoMore)
{
    auto value = question();
    value.peerProcess.clear();
    value.agentLabel.clear();
    const auto fixed = consentQuestionNotify(value);
    REQUIRE(fixed);
    // The process name's length varint grows from one byte to three.
    value.peerProcess.assign(maxFrameBodyBytes - fixed->size() - 2, 'x');
    REQUIRE(value.peerProcess.size() <= maxConsentIdentityBytes);
    const auto exact = consentQuestionNotify(value);
    REQUIRE(exact);
    CHECK(exact->size() == maxFrameBodyBytes);
    CHECK(decodeConsentQuestion(recordOf(exact, NotifyKind::ConsentQuestion))->peerProcess == value.peerProcess);
    value.peerProcess.push_back('x');
    CHECK(!consentQuestionNotify(value));
}

TEST(structureConsentCarriesTheExactOperationAndItsRetirementIsTheSharedOne)
{
    StructureConsent value { .requestID = 4, .sessionID = 5, .workspaceID = 3, .revision = UINT64_MAX, .workspaceName = "verified workspace", .method = "workspace.rename", .parameters = R"({"workspace":3,"name":"new"})", .agentLabel = "claimed helper", .peerProcess = "/verified/process", .peerProcessID = 23 };
    const auto record = recordOf(structureConsentNotify(value), NotifyKind::StructureConsent);
    CHECK(decodeStructureConsent(record) == value);
    for (std::size_t cut = 0; cut < record.size(); ++cut)
        CHECK(!decodeStructureConsent(std::span(record).first(cut)));
    using Change = void (*)(StructureConsent&);
    for (const Change invalid : std::initializer_list<Change> {
             [](StructureConsent& question) { question.requestID = 0; },
             [](StructureConsent& question) { question.sessionID = 0; },
             [](StructureConsent& question) { question.workspaceID = 0; },
             [](StructureConsent& question) { question.revision = 0; },
             [](StructureConsent& question) { question.peerProcessID = 0; },
             [](StructureConsent& question) { question.method = "pane.write"; },
             [](StructureConsent& question) { question.parameters = R"({"workspace":3})"; },
             [](StructureConsent& question) { question.parameters = R"({"workspace":3,"name":"new","extra":1})"; },
         }) {
        auto changed = value;
        invalid(changed);
        CHECK(!structureConsentNotify(changed));
    }
    const ConsentResolution resolution { ConsentKind::Structure, value.requestID, value.sessionID };
    CHECK(decodeConsentResolved(recordOf(consentResolvedNotify(resolution), NotifyKind::ConsentResolved)) == resolution);
    value.workspaceName.assign(4096, '\x01');
    value.parameters.resize(maxStructureConsentParametersBytes, ' ');
    value.agentLabel.assign(256, '\x01');
    value.peerProcess.assign(1024, '\x01');
    const auto maximum = structureConsentNotify(value);
    REQUIRE(maximum);
    CHECK(maximum->size() <= maxFrameBodyBytes);
    CHECK(decodeStructureConsent(recordOf(maximum, NotifyKind::StructureConsent)) == value);
    value.parameters.push_back(' ');
    CHECK(!structureConsentNotify(value));
}

int main(int argc, char** argv)
{
    return UbiquittyTest::runRegisteredCases(argc, argv);
}
