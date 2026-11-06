// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The socket fixtures' consent oracle is exercised only with owned memory.
// No daemon, descriptor, process, clock or transport is started here.

#include "Harness.h"
#include "Host/ConsentTranscript.h"

#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Notify.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Ubiquitty::HostTest::ConsentTranscript;
using namespace Ubiquitty::Protocol::Sync;
namespace JSON = Ubiquitty::Codec::JSON;
namespace Control = Ubiquitty::Protocol::Control;

constexpr ID128 hostID { 41, 42 };
constexpr ID128 attachmentID { 51, 52 };
constexpr ID128 paneID { 11, 12 };
constexpr ConsentBinding ownQuestion { .requestID = 7, .sessionID = 11 };

Welcome welcome(AttachmentAdmission admission)
{
    Welcome value;
    value.wireVersion = wireVersion;
    value.hostInstanceID = hostID;
    value.attachmentID = attachmentID;
    value.resumeSecret = { 61, 62 };
    value.grantedRole = Role::Agent;
    value.selection = { .target = AdmissionTarget::Session, .sessionID = 11, .workspaceID = 0, .daemonInstanceID = 42, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    value.admission = admission;
    if (admission == AttachmentAdmission::AwaitingConsent)
        value.consent = ownQuestion;
    value.principal = "verified test principal";
    value.limits.maxOutstandingCalls = 8;
    value.limits.maxQueuedBulkBytes = 4096;
    value.limits.maxPendingRanges = 4;
    return value;
}

template<typename Message>
std::vector<std::uint8_t> frame(FrameKind kind, const Message& message, std::uint32_t channel = 0)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    std::vector<std::uint8_t> bytes;
    CHECK(appendFrame(bytes, kind, channel, body));
    return bytes;
}

std::vector<std::uint8_t> notification(const std::vector<std::uint8_t>& body)
{
    std::vector<std::uint8_t> bytes;
    CHECK(appendFrame(bytes, FrameKind::Notify, 0, body));
    return bytes;
}

std::vector<std::uint8_t> status(ConsentStatusPhase phase, ConsentBinding question = ownQuestion)
{
    const auto body = consentStatusNotify({ .question = question, .phase = phase });
    CHECK(body);
    return body ? notification(*body) : std::vector<std::uint8_t> {};
}

bool feed(ConsentTranscript& transcript, const std::vector<std::uint8_t>& bytes)
{
    CHECK(!bytes.empty());
    return !bytes.empty() && transcript.feed(std::as_bytes(std::span(bytes)));
}

bool beginPending(ConsentTranscript& transcript)
{
    return feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent)))
        && feed(transcript, status(ConsentStatusPhase::Pending));
}

std::vector<std::uint8_t> result(std::string_view id, Control::ErrorCode error)
{
    const auto identity = JSON::Value::string(std::string(id));
    return frame(FrameKind::Result, Result { .transfer = 0, .text = JSON::serialize(Control::rpcError(&identity, error, "The exact target is unavailable.")) });
}

std::vector<std::uint8_t> successfulResult(std::string_view id)
{
    const auto identity = JSON::Value::string(std::string(id));
    return frame(FrameKind::Result, Result { .transfer = 0, .text = JSON::serialize(Control::rpcResult(&identity, JSON::Value::object())) });
}

} // namespace

TEST(aConsentTranscriptAcceptsGrantedWelcomeWithoutPendingStatus)
{
    ConsentTranscript transcript;
    CHECK(!transcript.isGranted());
    const auto bytes = frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted));
    REQUIRE(bytes.size() > 1);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        REQUIRE(transcript.feed(std::as_bytes(std::span(bytes).subspan(index, 1))));
        CHECK(transcript.isGranted() == (index + 1 == bytes.size()));
    }
    CHECK(!transcript.hasFailed());
    CHECK(!transcript.takeStatus(ConsentStatusPhase::Pending));
    CHECK(!transcript.takeStatus(ConsentStatusPhase::Approved));
    REQUIRE(feed(transcript, frame(FrameKind::PaneBind, PaneBind { .handle = 1, .paneID = paneID })));
    CHECK(transcript.isGranted());
}

TEST(aPendingConsentTranscriptBecomesGrantedOnlyAfterTheExactApproval)
{
    ConsentTranscript transcript;
    REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent))));
    CHECK(!transcript.isGranted());
    REQUIRE(feed(transcript, frame(FrameKind::Probe, Probe { .time = 123 })));
    REQUIRE(feed(transcript, status(ConsentStatusPhase::Pending)));
    CHECK(!transcript.isGranted());
    CHECK(transcript.takeStatus(ConsentStatusPhase::Pending) == (ConsentStatus { .question = ownQuestion, .phase = ConsentStatusPhase::Pending }));
    REQUIRE(feed(transcript, status(ConsentStatusPhase::Approved)));
    CHECK(transcript.takeStatus(ConsentStatusPhase::Approved) == (ConsentStatus { .question = ownQuestion, .phase = ConsentStatusPhase::Approved }));
    CHECK(transcript.isGranted());
    // Production promotes the original Welcome after Approved; it sends no
    // second Welcome before binding the pane.
    REQUIRE(feed(transcript, frame(FrameKind::PaneBind, PaneBind { .handle = 1, .paneID = paneID })));
    REQUIRE(feed(transcript, frame(FrameKind::CheckpointHead, CheckpointHead { .generation = 1, .transfer = 3, .frameClass = FrameClass::Structural, .stateDigest = {} }, 1)));
    CHECK(!transcript.hasFailed());
}

TEST(aConsentTranscriptDoesNotRepairPendingThatPrecedesWelcome)
{
    auto bytes = status(ConsentStatusPhase::Pending);
    const auto lateWelcome = frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent));
    bytes.insert(bytes.end(), lateWelcome.begin(), lateWelcome.end());
    ConsentTranscript transcript;
    CHECK(!feed(transcript, bytes));
    CHECK(transcript.hasFailed());
    CHECK(!transcript.takeStatus(ConsentStatusPhase::Pending));
    CHECK(!feed(transcript, lateWelcome));
    CHECK(!transcript.isGranted());
}

TEST(aConsentTranscriptRejectsMissingOrLateWelcomeBeforeConnectionData)
{
    const std::array premature {
        frame(FrameKind::PaneBind, PaneBind { .handle = 1, .paneID = paneID }),
        frame(FrameKind::CheckpointHead, CheckpointHead { .generation = 1, .transfer = 3, .frameClass = FrameClass::Structural, .stateDigest = {} }, 1),
        successfulResult("before-welcome"),
    };
    for (const auto& bytes : premature) {
        ConsentTranscript transcript;
        CHECK(!feed(transcript, bytes));
        CHECK(transcript.hasFailed());
        CHECK(!feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted))));
        CHECK(!transcript.isGranted());
    }
}

TEST(aConsentTranscriptRejectsTerminalStatusBeforePending)
{
    for (const auto phase : { ConsentStatusPhase::Approved, ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        auto bytes = frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent));
        const auto terminal = status(phase);
        const auto latePending = status(ConsentStatusPhase::Pending);
        bytes.insert(bytes.end(), terminal.begin(), terminal.end());
        bytes.insert(bytes.end(), latePending.begin(), latePending.end());
        ConsentTranscript transcript;
        CHECK(!feed(transcript, bytes));
        CHECK(transcript.hasFailed());
        CHECK(!transcript.takeStatus(phase));
        CHECK(!transcript.takeStatus(ConsentStatusPhase::Pending));
        CHECK(!transcript.isGranted());
    }
}

TEST(aConsentTranscriptRefusesPaneDataAtEitherPendingBoundary)
{
    const std::array paneData {
        frame(FrameKind::PaneBind, PaneBind { .handle = 1, .paneID = paneID }),
        frame(FrameKind::CheckpointHead, CheckpointHead { .generation = 1, .transfer = 3, .frameClass = FrameClass::Structural, .stateDigest = {} }, 1),
    };
    for (const auto& bytes : paneData) {
        for (unsigned pendingCount : { 0u, 1u }) {
            ConsentTranscript transcript;
            REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent))));
            if (pendingCount)
                REQUIRE(feed(transcript, status(ConsentStatusPhase::Pending)));
            CHECK(!feed(transcript, bytes));
            CHECK(transcript.hasFailed());
            CHECK(!feed(transcript, status(ConsentStatusPhase::Approved)));
            CHECK(!transcript.isGranted());
        }
    }
}

TEST(aConsentTranscriptRefusesOtherValidControlTrafficAtEitherPendingBoundary)
{
    const ConsentQuestion question { .requestID = ownQuestion.requestID, .sessionID = ownQuestion.sessionID, .peerProcessID = 23, .peerProcess = "/verified/tool", .agentLabel = "claimed label", .action = std::to_underlying(ConsentKind::Attach) };
    const auto asked = consentQuestionNotify(question);
    const auto resolved = consentResolvedNotify({ .kind = ConsentKind::Attach, .requestID = ownQuestion.requestID, .sessionID = ownQuestion.sessionID });
    const AttachmentsList roster { .revision = 3, .attachments = { AttachmentEntry { .attachmentID = attachmentID, .role = AttachmentRole::Observer, .clientKind = ClientKind::Human, .state = AttachmentState::Live, .panes = { paneID }, .isSelf = true, .principal = std::nullopt } } };
    const auto listed = attachmentsNotify(roster);
    REQUIRE(asked && resolved && listed);
    const std::array otherControl {
        notification(*asked),
        notification(*resolved),
        notification(*listed),
        successfulResult("consent:answer:1"),
    };
    // These are accepted after a normal human admission. Their pending-state
    // rejection must not be a malformed-body rejection in disguise.
    ConsentTranscript admitted;
    auto humanWelcome = welcome(AttachmentAdmission::Granted);
    humanWelcome.grantedRole = Role::Owner;
    REQUIRE(feed(admitted, frame(FrameKind::Welcome, humanWelcome)));
    for (const auto& bytes : otherControl)
        REQUIRE(feed(admitted, bytes));
    CHECK(admitted.takeResult("consent:answer:1"));
    CHECK(admitted.isGranted());
    for (const auto& bytes : otherControl) {
        for (unsigned pendingCount : { 0u, 1u }) {
            ConsentTranscript transcript;
            REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent))));
            if (pendingCount)
                REQUIRE(feed(transcript, status(ConsentStatusPhase::Pending)));
            CHECK(!feed(transcript, bytes));
            CHECK(transcript.hasFailed());
            CHECK(!transcript.takeQuestion(ConsentKind::Attach));
            CHECK(!transcript.takeRoster());
            CHECK(!transcript.takeResult("consent:answer:1"));
            CHECK(!feed(transcript, status(ConsentStatusPhase::Approved)));
            CHECK(!transcript.isGranted());
        }
    }
}

TEST(aConsentTranscriptPinsItsExactRequestKindAndSession)
{
    for (const ConsentBinding other : { ConsentBinding { .requestID = 8, .sessionID = ownQuestion.sessionID }, ConsentBinding { .requestID = ownQuestion.requestID, .sessionID = 12 } }) {
        for (const auto phase : { ConsentStatusPhase::Pending, ConsentStatusPhase::Approved }) {
            ConsentTranscript transcript;
            REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::AwaitingConsent))));
            if (phase == ConsentStatusPhase::Approved)
                REQUIRE(feed(transcript, status(ConsentStatusPhase::Pending)));
            CHECK(!feed(transcript, status(phase, other)));
            CHECK(transcript.hasFailed());
            CHECK(!transcript.takeStatus(phase));
            CHECK(!transcript.isGranted());
        }
    }
    // A status record has no kind of its own: one with a phase past Expired,
    // or with a byte more or less, is no status at all.
    auto approved = *consentStatusNotify({ .question = ownQuestion, .phase = ConsentStatusPhase::Approved });
    auto unknownPhase = approved;
    unknownPhase.back() = 4;
    auto extended = approved;
    extended.push_back(0);
    auto truncated = approved;
    truncated.pop_back();
    for (const auto& body : { unknownPhase, extended, truncated }) {
        ConsentTranscript transcript;
        REQUIRE(beginPending(transcript));
        CHECK(!feed(transcript, notification(body)));
        CHECK(transcript.hasFailed());
        CHECK(!transcript.isGranted());
    }
}

TEST(aConsentTranscriptCannotReplaceItsWelcomedHostOrAttachment)
{
    // Own status carries request/session, not independently claimed host or
    // attachment fields: those remain bound to the one physical Welcome.
    for (unsigned replacement : { 0u, 1u, 2u }) {
        ConsentTranscript transcript;
        REQUIRE(beginPending(transcript));
        auto changed = welcome(AttachmentAdmission::AwaitingConsent);
        if (replacement == 1)
            changed.hostInstanceID = { 43, 44 };
        else if (replacement == 2)
            changed.attachmentID = { 53, 54 };
        CHECK(!feed(transcript, frame(FrameKind::Welcome, changed)));
        CHECK(transcript.hasFailed());
        CHECK(!feed(transcript, status(ConsentStatusPhase::Approved)));
        CHECK(!transcript.isGranted());
    }
}

TEST(aConsentTranscriptDoesNotRepeatPendingOrTerminalTransitions)
{
    for (const auto phase : { ConsentStatusPhase::Pending, ConsentStatusPhase::Approved, ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        ConsentTranscript transcript;
        REQUIRE(beginPending(transcript));
        if (phase != ConsentStatusPhase::Pending)
            REQUIRE(feed(transcript, status(phase)));
        CHECK(!feed(transcript, status(phase)));
        CHECK(transcript.hasFailed());
        CHECK(!transcript.isGranted());
    }
}

TEST(aConsentTranscriptRequiresNoConsentByeAfterDenialOrExpiry)
{
    for (const auto phase : { ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        ConsentTranscript transcript;
        REQUIRE(beginPending(transcript));
        REQUIRE(feed(transcript, status(phase)));
        CHECK(transcript.takeStatus(phase) == (ConsentStatus { .question = ownQuestion, .phase = phase }));
        CHECK(!transcript.isGranted());
        REQUIRE(feed(transcript, frame(FrameKind::Bye, Bye { .reason = ByeReason::NoConsent, .detail = {} })));
        CHECK(!transcript.hasFailed());
        CHECK(!feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted))));
        CHECK(transcript.hasFailed());
        CHECK(!transcript.isGranted());

        ConsentTranscript wrongFarewell;
        REQUIRE(beginPending(wrongFarewell));
        REQUIRE(feed(wrongFarewell, status(phase)));
        CHECK(!feed(wrongFarewell, frame(FrameKind::Bye, Bye { .reason = ByeReason::Normal, .detail = {} })));
        CHECK(wrongFarewell.hasFailed());
    }
}

TEST(aConsentTranscriptNeverAdmitsDataAfterTerminalRefusal)
{
    const std::array forbidden {
        frame(FrameKind::PaneBind, PaneBind { .handle = 1, .paneID = paneID }),
        frame(FrameKind::CheckpointHead, CheckpointHead { .generation = 1, .transfer = 3, .frameClass = FrameClass::Structural, .stateDigest = {} }, 1),
        successfulResult("late-answer"),
    };
    for (const auto phase : { ConsentStatusPhase::Denied, ConsentStatusPhase::Expired }) {
        for (const auto& bytes : forbidden) {
            ConsentTranscript transcript;
            REQUIRE(beginPending(transcript));
            REQUIRE(feed(transcript, status(phase)));
            CHECK(!feed(transcript, bytes));
            CHECK(transcript.hasFailed());
            CHECK(!transcript.isGranted());
        }
    }
}

TEST(aConsentTranscriptKeepsUnknownAndUnrelatedResultsSeparate)
{
    ConsentTranscript transcript;
    REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted))));
    REQUIRE(feed(transcript, successfulResult("consent:revoke:10")));
    CHECK(!transcript.takeResult("consent:revoke:1"));
    CHECK(!transcript.takeResult("unknown-call"));
    REQUIRE(feed(transcript, result("consent:revoke:1", Control::ErrorCode::NotFound)));
    const auto denied = transcript.takeResult("consent:revoke:1");
    REQUIRE(denied);
    CHECK(denied->stringOr("id") == "consent:revoke:1");
    const auto* error = denied->find("error");
    REQUIRE(error);
    CHECK(error->numberOr("code") == std::to_underlying(Control::ErrorCode::NotFound));
    CHECK(!denied->find("result"));
    CHECK(!transcript.takeResult("consent:revoke:1"));
    const auto unrelated = transcript.takeResult("consent:revoke:10");
    REQUIRE(unrelated);
    CHECK(unrelated->find("result"));
    CHECK(!unrelated->find("error"));
    CHECK(!transcript.takeResult("consent:revoke:10"));
    CHECK(transcript.isGranted());
    CHECK(!transcript.hasFailed());
}

TEST(aConsentTranscriptRefusesAmbiguousOrBulkConsentResults)
{
    for (const char* text : {
             R"({"jsonrpc":"2.0","id":"answer","result":{},"error":{"code":-32003,"message":"No target."}})",
             R"({"jsonrpc":"2.0","id":"answer"})",
             R"({"jsonrpc":"2.0","id":1,"result":{}})",
             R"({"jsonrpc":"2.0","id":"answer","id":"another","result":{}})",
             R"({"jsonrpc":"1.0","id":"answer","result":{}})",
             R"({"jsonrpc":"2.0","id":"answer","error":{"code":-32003,"code":-32002,"message":"No target."}})",
             R"({"jsonrpc":"2.0","id":"answer","error":{"code":-32003,"message":"No target.","unknown":true}})",
             R"({"jsonrpc":"2.0","id":"answer","error":{"code":-32003.5,"message":"No target."}})",
             R"({"jsonrpc":"2.0","id":"answer","error":{"code":2147483648,"message":"No target."}})",
             "not json" }) {
        ConsentTranscript transcript;
        REQUIRE(feed(transcript, frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted))));
        CHECK(!feed(transcript, frame(FrameKind::Result, Result { .transfer = 0, .text = text })));
        CHECK(transcript.hasFailed());
        CHECK(!transcript.takeResult("answer"));
    }
    ConsentTranscript bulk;
    REQUIRE(feed(bulk, frame(FrameKind::Welcome, welcome(AttachmentAdmission::Granted))));
    CHECK(!feed(bulk, frame(FrameKind::Result, Result { .transfer = 7, .text = {} })));
    CHECK(bulk.hasFailed());
}

TEST(aGrantedConsentTranscriptKeepsHumanQuestionsAndRosterIndependent)
{
    ConsentTranscript transcript;
    auto humanWelcome = welcome(AttachmentAdmission::Granted);
    humanWelcome.grantedRole = Role::Owner;
    REQUIRE(feed(transcript, frame(FrameKind::Welcome, humanWelcome)));
    ConsentQuestion question { .requestID = 21, .sessionID = 11, .peerProcessID = 23, .peerProcess = "/verified/tool", .agentLabel = "claimed label", .action = std::to_underlying(ConsentKind::Terminate) };
    const auto asked = consentQuestionNotify(question);
    REQUIRE(asked);
    REQUIRE(feed(transcript, notification(*asked)));
    const AttachmentsList roster { .revision = 3, .attachments = { AttachmentEntry { .attachmentID = attachmentID, .role = AttachmentRole::Observer, .clientKind = ClientKind::Human, .state = AttachmentState::Live, .panes = { paneID }, .isSelf = true, .principal = std::nullopt } } };
    const auto listed = attachmentsNotify(roster);
    REQUIRE(listed);
    REQUIRE(feed(transcript, notification(*listed)));
    CHECK(!transcript.takeQuestion(ConsentKind::Attach));
    const auto received = transcript.takeQuestion(ConsentKind::Terminate);
    REQUIRE(received);
    CHECK(received->requestID == question.requestID && received->sessionID == question.sessionID);
    CHECK(received->action == question.action);
    CHECK(!transcript.takeQuestion(ConsentKind::Terminate));
    const auto receivedRoster = transcript.takeRoster();
    REQUIRE(receivedRoster);
    CHECK(receivedRoster->revision == roster.revision);
    REQUIRE(receivedRoster->attachments.size() == 1);
    CHECK(receivedRoster->attachments.front().attachmentID == attachmentID);
    CHECK(!transcript.takeRoster());
    CHECK(transcript.isGranted());
    CHECK(!transcript.hasFailed());
}

UBIQUITTY_TEST_MAIN()
