// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_control.cpp — calls on the control channel: `Call`, `Result` and
// `Notify`, and the per-attachment caps that bound what a caller can make the
// host hold (Docs/sync.md §6.5, §7.3, §7.9).
//
// The properties held:
//
// - At most 32 calls are outstanding per attachment; the rest are answered
//   `busy` and the connection stays. Unanswered and queued answered Calls
//   count against the same cap.
// - An answer that would take the queued bulk past 16 MiB is answered `busy`.
// - A client that writes calls and reads nothing holds the host to the
//   interactive bound, because the owner stops reading it.
// - An answer larger than a frame travels as a transfer the `Result`
//   announces; an unreadable call draws the error that says why.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/App/SessionCreationText.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Security/StructureConsentText.h"
#include "Core/Session/NameLimits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using Ubiquitty::Codec::JSON::Value;
using UbiquittyTest::SyncLink;

namespace {

/// The record after a whole `Notify` body's kind, which must be `kind`.
std::vector<std::uint8_t> recordOf(const std::optional<std::vector<std::uint8_t>>& body, NotifyKind kind)
{
    const auto head = body ? decodeNotifyHead(*body) : std::nullopt;
    CHECK(head && head->kind == std::to_underlying(kind));
    return head ? std::vector<std::uint8_t>(head->record.begin(), head->record.end()) : std::vector<std::uint8_t> {};
}

/// The policy whose caps the tests read.
const HostPolicy policy = UbiquittyTest::syncHostPolicy(0);

/// A `pane.capture` request with an id.
///
/// - Parameter id: The request's id.
/// - Returns: Its text.
std::string captureRequest(int id)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"pane.capture","params":{"pane":1}})";
}

/// The error code a response carries, or 0 for a result.
///
/// - Parameter response: A JSON-RPC 2.0 response's text.
/// - Returns: The code.
int errorCode(const std::string& response)
{
    bool ok = false;
    const Value parsed = Ubiquitty::Codec::JSON::parse(response, ok);
    if (!ok || !parsed.find("error"))
        return 0;
    return static_cast<int>(parsed.find("error")->find("code")->asNumber());
}

/// The id a response answers, or 0 when it names none.
///
/// - Parameter response: A JSON-RPC 2.0 response's text.
/// - Returns: The id.
int callID(const std::string& response)
{
    bool ok = false;
    const Value parsed = Ubiquitty::Codec::JSON::parse(response, ok);
    const Value* id = ok ? parsed.find("id") : nullptr;
    return id && id->isNumber() ? static_cast<int>(id->asNumber()) : 0;
}

/// Answers every call the host has taken that is over a cap or unreadable,
/// and keeps the admitted ones unanswered: an owner that is slow to answer.
///
/// - Parameters:
///   - host: The host's end.
///   - admitted: Receives the admitted calls.
void refuseOnly(HostConnection& host, std::vector<ReceivedCall>& admitted)
{
    for (ReceivedCall& call : host.takeCalls()) {
        const Ubiquitty::Protocol::Control::DecodedRequest request = decodeCallRequest(call.text);
        if (call.admission == CallAdmission::OverLimit)
            host.refuseCall(call.callIndex, busyResponse(request.idOrNull()), CallRefusal::Busy);
        else
            admitted.push_back(std::move(call));
    }
}

/// A method that answers every request with a result of `bytes` characters.
///
/// - Parameter bytes: The result's length.
/// - Returns: The server.
CallServer answeringWith(std::size_t bytes)
{
    return [bytes](const Ubiquitty::Protocol::Control::DecodedRequest& request) {
        return Ubiquitty::Protocol::Control::rpcResult(request.idOrNull(), Value::string(std::string(bytes, 'x')));
    };
}

} // namespace

TEST(callSizeRefusalNamesTheSerializedByteBound)
{
    CHECK(!callSizeRefusal(0));
    CHECK(!callSizeRefusal(maxFrameBodyBytes));
    const auto refusal = callSizeRefusal(maxFrameBodyBytes + 1);
    REQUIRE(refusal);
    CHECK_STR_EQ(*refusal, "A host call holds at most 65535 bytes, and this one holds 65536.");

    const std::size_t largest = std::numeric_limits<std::size_t>::max();
    const auto largestRefusal = callSizeRefusal(largest);
    REQUIRE(largestRefusal);
    CHECK_STR_EQ(*largestRefusal, "A host call holds at most 65535 bytes, and this one holds " + std::to_string(largest) + ".");
}

TEST(aDeferredCallStaysOutstandingUntilItsSingleAnswer)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    REQUIRE(link.client().call(captureRequest(1)));
    link.step();
    std::uint32_t held = 0;
    serveCallsDeferred(link.host(), [&](std::uint32_t index, const Ubiquitty::Protocol::Control::DecodedRequest&) -> std::optional<Value> {
        held = index;
        return std::nullopt;
    });
    REQUIRE(held != 0);
    link.stepUntilQuiet();
    CHECK(link.client().takeResults().empty());
    CHECK(link.host().answerCall(held, R"({"jsonrpc":"2.0","id":1,"result":{}})") == CallAnswer::Sent);
    CHECK(link.host().answerCall(held, R"({"jsonrpc":"2.0","id":1,"result":{}})") == CallAnswer::NotOutstanding);
    link.stepUntilQuiet();
    CHECK_EQ(link.client().takeResults().size(), 1u);
}

TEST(structureConsentNamesEveryOperationAndItsExactParameters)
{
    const std::pair<std::string, std::string> operations[] = {
        { "label.set", R"({"object":"pane:3","label":"logs"})" },
        { "tags.set", R"({"object":"pane:3","tags":["logs"]})" },
        { "workspace.rename", R"({"workspace":3,"name":"logs"})" },
        { "tab.rename", R"({"tab":3,"name":"logs"})" },
        { "tab.move", R"({"tab":3,"window":4,"index":0})" },
        { "window.close", R"({"window":4,"expectedRevision":"7"})" },
        { "window.activate", R"({"window":4})" },
        { "tab.activate", R"({"tab":3})" },
        { "pane.focus", R"({"pane":2})" },
        { "window.moveFocus", R"({"window":4,"pane":2,"direction":"left","expectedRevision":"7"})" },
        { "pane.zoom", R"({"pane":2,"state":"zoomed","expectedRevision":"7"})" },
        { "pane.move", R"({"window":4,"pane":2,"target":3,"orientation":"vertical","side":"after","expectedRevision":"7"})" },
        { "pane.swap", R"({"window":4,"pane":2,"target":3,"expectedRevision":"7"})" },
        { "pane.extractTab", R"({"window":4,"pane":2,"expectedRevision":"7"})" },
        { "pane.extractWindow", R"({"window":4,"pane":2,"expectedRevision":"7"})" },
        { "tab.extractWindow", R"({"window":4,"tab":3,"expectedRevision":"7"})" },
        { "pane.moveToTab", R"({"window":4,"pane":2,"tab":3,"target":4,"orientation":"vertical","side":"after","insertTarget":"root","expectedRevision":"7"})" },
        { "tab.moveToPane", R"({"window":4,"tab":2,"target":4,"orientation":"vertical","side":"after","insertTarget":"pane","expectedRevision":"7"})" },
        { "pane.moveToNewTab", R"({"window":4,"pane":2,"targetWindow":3,"expectedRevision":"7"})" },
        { "workspace.addWindow", R"({"window":4,"pane":2,"expectedRevision":"7"})" },
        { "tab.balance", R"({"window":4,"tab":2,"expectedRevision":"7"})" },
        { "pane.adjustRatio", R"({"window":4,"tab":2,"pane":3,"direction":"left","step":0.1,"expectedRevision":"7"})" },
        { "divider.setRatio", R"({"window":4,"tab":2,"path":[],"ratio":0.4,"expectedRevision":"7"})" },
        { "workspace.addTab", R"({"window":4})" },
        { "workspace.split", R"({"pane":3,"orientation":"horizontal","ratio":0.375})" },
        { "workspace.closeTab", R"({"tab":3})" },
        { "workspace.closePane", R"({"pane":3})" },
    };
    for (const auto& [method, parameters] : operations) {
        bool isJSON = false; // bool-ok: parser output
        const auto parsedParameters = Ubiquitty::Codec::JSON::parse(parameters, isJSON);
        REQUIRE(isJSON);
        const StructureConsent request {
            .requestID = 4,
            .sessionID = 5,
            .workspaceID = 3,
            .revision = std::numeric_limits<std::uint64_t>::max(),
            .workspaceName = "work",
            .method = method,
            .parameters = Ubiquitty::Codec::JSON::serialize(parsedParameters),
            .agentLabel = "helper",
            .peerProcess = "/usr/bin/tool",
            .peerProcessID = 7,
        };
        const auto read = decodeStructureConsent(recordOf(structureConsentNotify(request), NotifyKind::StructureConsent));
        REQUIRE(read);
        CHECK(*read == request);
        const auto words = Ubiquitty::Security::structureConsentText(*read);
        CHECK(!words.question.empty());
        CHECK(words.detail.contains("only this change"));
        if (method == "workspace.split")
            CHECK(words.detail.contains("horizontal") && words.detail.contains("0.375"));
        CHECK(createsSessionForStructure(method) == (method == "workspace.addWindow" || method == "workspace.addTab" || method == "workspace.split"));
        CHECK_STR_EQ(words.allow, "Allow Once");
        CHECK_STR_EQ(words.deny, "Deny");
    }
}

TEST(structureConsentFitsWorstEscapedNamesAndSanitizesOnlyTheirDisplay)
{
    Value parameters = Value::object();
    parameters.set("workspace", Value::number(3));
    const std::string name(Ubiquitty::Session::maxWorkspaceNameBytes, '\x01');
    parameters.set("name", Value::string(name));
    StructureConsent request {
        .requestID = 4,
        .sessionID = 5,
        .workspaceID = 3,
        .revision = 1,
        .workspaceName = name,
        .method = "workspace.rename",
        .parameters = Ubiquitty::Codec::JSON::serialize(parameters),
        .agentLabel = std::string(256, '\x01'),
        .peerProcess = std::string(1024, '\x01'),
        .peerProcessID = 7,
    };
    REQUIRE(request.parameters.size() < maxStructureConsentParametersBytes);
    const auto body = structureConsentNotify(request);
    REQUIRE(body);
    CHECK(body->size() < maxFrameBodyBytes);
    CHECK(decodeStructureConsent(recordOf(body, NotifyKind::StructureConsent)) == request);
    request.workspaceName = "safe\n\x1b[31m\xE2\x80\xAE";
    const auto words = Ubiquitty::Security::structureConsentText(request);
    CHECK(!words.detail.contains('\x01'));
    CHECK(!words.detail.contains('\x1b'));
    CHECK(!words.detail.contains("\xE2\x80\xAE"));
    CHECK(!words.detail.contains("safe\n"));
    request.parameters = std::string(maxStructureConsentParametersBytes + 1, ' ');
    CHECK(!structureConsentNotify(request));
}

TEST(aStructureConsentRetirementNamesOnlyAnExistingQuestion)
{
    const ConsentResolution resolution { .kind = ConsentKind::Structure, .requestID = UINT32_MAX, .sessionID = 7 };
    const auto body = consentResolvedNotify(resolution);
    REQUIRE(body);
    CHECK(body->size() == maxConsentResolutionBytes);
    const auto record = recordOf(body, NotifyKind::ConsentResolved);
    CHECK(decodeConsentResolved(record) == resolution);
    CHECK(!decodeStructureConsent(record));
    CHECK(!consentResolvedNotify({ .kind = ConsentKind::Structure, .requestID = 0, .sessionID = 7 }));
    CHECK(!consentResolvedNotify({ .kind = ConsentKind::Structure, .requestID = 1, .sessionID = 0 }));
    auto extended = record;
    extended.push_back(0);
    CHECK(!decodeConsentResolved(extended));
}

TEST(treeCallsWaitForTheHostDecisionAndPartialCreationNeverClaimsSuccess)
{
    for (std::string_view method : { "label.set", "tags.set", "workspace.rename", "tab.rename", "tab.move", "window.close", "workspace.addWindow", "workspace.addTab", "workspace.split", "workspace.closeTab", "workspace.closePane",
             "window.activate", "tab.activate", "pane.focus", "window.moveFocus", "pane.zoom", "pane.move", "pane.swap",
             "pane.extractTab", "pane.extractWindow", "tab.extractWindow",
             "pane.moveToTab", "tab.moveToPane", "pane.moveToNewTab", "tab.balance", "pane.adjustRatio", "divider.setRatio" })
        CHECK(hostCallWaitMilliseconds(method) > static_cast<int>(structureConsentTimeoutMilliseconds));
    CHECK_EQ(hostCallWaitMilliseconds("host.describe"), 5000);
    for (std::string_view reason : { "an attached person denied this exact tree change", "approval expired without permission", "no such person is attached", "the daemon did not answer" }) {
        const auto withoutCommand = Ubiquitty::App::sessionCreatedWithoutLabelText(7, 9, reason, false);
        CHECK(withoutCommand.contains("session:7") && withoutCommand.contains("pane:9"));
        CHECK(withoutCommand.contains("label was not applied") && withoutCommand.contains("remains available"));
        CHECK(withoutCommand.contains(reason) && !withoutCommand.contains("command"));
        const auto withCommand = Ubiquitty::App::sessionCreatedWithoutLabelText(7, 9, reason, true);
        CHECK(withCommand.contains("requested command was not started"));
    }
}

// The §10 negative: 1,032 calls from a client whose owner
// answers none. 32 are admitted, the other 1,000 are answered busy, and the
// connection stays; once one answer has left, the next call is admitted.
TEST(callsBeyondTheLimitAreAnsweredBusy)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    std::vector<ReceivedCall> admitted;
    for (int id = 1; id <= 1'032; ++id) {
        REQUIRE(link.client().call(captureRequest(id)));
        if (id % 64 == 0) {
            link.step();
            refuseOnly(link.host(), admitted);
        }
    }
    link.step();
    refuseOnly(link.host(), admitted);
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().callsAdmitted, std::uint64_t { 32 });
    CHECK_EQ(admitted.size(), std::size_t { 32 });
    const std::vector<std::string> results = link.client().takeResults();
    CHECK_EQ(results.size(), std::size_t { 1'000 });
    CHECK(std::ranges::all_of(results, [](const std::string& result) { return errorCode(result) == -32005; }));
    CHECK_EQ(link.host().counters().callsAnsweredBusy, std::uint64_t { 1'000 });
    CHECK(link.host().state() == ConnectionState::Attached);

    REQUIRE(link.host().answerCall(admitted.front().callIndex, R"({"jsonrpc":"2.0","id":1,"result":"ok"})") == CallAnswer::Sent);
    link.stepUntilQuiet();
    REQUIRE(link.client().call(captureRequest(2'000)));
    link.step();
    std::vector<ReceivedCall> next;
    refuseOnly(link.host(), next);
    CHECK_EQ(next.size(), std::size_t { 1 });
}

// Two answers of 7 MiB are queued (14 MiB within 16); the third would pass the
// queued-bulk cap and is answered busy, though only three calls are out.
TEST(aQueuedBulkBoundRefusesTheNextCall)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdDownstream();
    for (int id = 1; id <= 3; ++id)
        REQUIRE(link.client().call(captureRequest(id)));
    link.step();
    serveCalls(link.host(), answeringWith(std::size_t { 7 } << 20));
    CHECK_EQ(link.host().counters().callsAdmitted, std::uint64_t { 3 });
    CHECK_EQ(link.host().counters().callsAnsweredBusy, std::uint64_t { 1 });
    CHECK(link.host().queuedBulkBytes() <= policy.limits.maxQueuedBulkBytes);

    link.releaseDownstream();
    link.stepUntilQuiet(100'000);
    const std::vector<std::string> results = link.client().takeResults();
    REQUIRE(results.size() == 3);
    CHECK_EQ(std::ranges::count_if(results, [](const std::string& result) { return errorCode(result) == -32005; }), 1);
    CHECK(std::ranges::count_if(results, [](const std::string& result) { return result.size() > (std::size_t { 7 } << 20); }) == 2);
    CHECK(link.host().state() == ConnectionState::Attached);
}

// A client writes 100,000 calls and reads nothing, so nothing the host queues
// leaves. The owner reads the client only while the connection wants input,
// which holds the unsent interactive bytes to the bound and one batch of reads.
TEST(aNonReadingCallerHoldsAtMostTheInteractiveBound)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    const auto methodNotFound = [](const Ubiquitty::Protocol::Control::DecodedRequest& request) {
        return Ubiquitty::Protocol::Control::rpcError(request.idOrNull(), Ubiquitty::Protocol::Control::ErrorCode::MethodNotFound, "no such method");
    };
    constexpr int batch = 100;
    int written = 0;
    bool wasRefused = false;
    std::size_t peak = 0;
    while (written < 100'000) {
        if (!link.host().wantsInput()) {
            wasRefused = true;
            break;
        }
        for (int call = 0; call < batch; ++call) {
            std::vector<std::uint8_t> body;
            encode(body, Call { .text = captureRequest(++written) });
            link.sendToHost(FrameKind::Call, body);
        }
        serveCalls(link.host(), methodNotFound);
        peak = std::max(peak, link.host().queuedInteractiveBytes());
    }
    CHECK(wasRefused);
    CHECK_MSG(peak <= policy.maxQueuedInteractiveBytes + maxEnvelopeHeaderBytes + maxFrameBodyBytes,
        ("the host held " + std::to_string(peak) + " interactive bytes for a caller that reads nothing").c_str());
    CHECK(link.host().state() == ConnectionState::Attached);
}

// Queued answers and unanswered Calls share one count: thirty-two split
// across the two states make the next Call busy.
TEST(thirtyTwoQueuedRepliesAndUnansweredCallsMakeTheNextCallBusy)
{
    for (const int answeredReplies : { 0, 16, 31 }) {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        (void)link.addPane();
        for (int index = 0; index < answeredReplies; ++index) {
            std::vector<std::uint8_t> body;
            encode(body, Call { .text = captureRequest(1000 + index) });
            link.sendToHost(FrameKind::Call, body);
            const auto calls = link.host().takeCalls();
            REQUIRE(calls.size() == 1 && calls.front().admission == CallAdmission::Admitted);
            REQUIRE(link.host().answerCall(calls.front().callIndex,
                        R"({"jsonrpc":"2.0","id":)" + std::to_string(1000 + index) + R"(,"result":{}})")
                == CallAnswer::Sent);
        }
        std::vector<ReceivedCall> admitted;
        for (int id = 1; id <= 32 - answeredReplies; ++id) {
            std::vector<std::uint8_t> body;
            encode(body, Call { .text = captureRequest(id) });
            link.sendToHost(FrameKind::Call, body);
        }
        refuseOnly(link.host(), admitted);
        CHECK_EQ(admitted.size(), std::size_t(32 - answeredReplies));
        CHECK(!link.host().admitsCall(0));

        std::vector<std::uint8_t> body;
        encode(body, Call { .text = captureRequest(100) });
        link.sendToHost(FrameKind::Call, body);
        const std::vector<ReceivedCall> over = link.host().takeCalls();
        REQUIRE(over.size() == 1);
        CHECK(over.front().admission == CallAdmission::OverLimit);
    }
}

// An answer larger than a frame travels as a transfer its Result announces,
// and the client hands it over whole.
TEST(anAnswerLargerThanAFrameArrivesWhole)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    REQUIRE(link.client().call(captureRequest(7)));
    link.step();
    serveCalls(link.host(), answeringWith(200'000));
    link.stepUntilQuiet();
    const std::vector<std::string> results = link.client().takeResults();
    REQUIRE(results.size() == 1);
    bool ok = false;
    const Value parsed = Ubiquitty::Codec::JSON::parse(results.front(), ok);
    REQUIRE(ok);
    CHECK_EQ(parsed.find("result")->asString().size(), std::size_t { 200'000 });
    CHECK(link.client().transfersHeld() == 0);
}

// Text that is not a request draws a parse error and the connection stays; a
// request with no id is not a call; a body over the frame cap ends the
// connection before it is read.
TEST(anUnreadableCallIsAnsweredAndAnOversizedOneCloses)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    REQUIRE(link.client().call("{not json"));
    REQUIRE(link.client().call(R"({"jsonrpc":"2.0","method":"pane.capture"})"));
    link.step();
    serveCalls(link.host(), answeringWith(1));
    link.stepUntilQuiet();
    const std::vector<std::string> results = link.client().takeResults();
    REQUIRE(results.size() == 2);
    CHECK_EQ(errorCode(results[0]), -32700);
    CHECK_EQ(errorCode(results[1]), -32600);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(!link.client().call(std::string(maxFrameBodyBytes + 1, 'x')));

    std::vector<std::uint8_t> oversized { static_cast<std::uint8_t>(FrameKind::Call), 0 };
    for (std::uint64_t length = maxFrameBodyBytes + 1; length >= 0x80; length >>= 7)
        oversized.push_back(static_cast<std::uint8_t>(0x80 | (length & 0x7F)));
    oversized.push_back(static_cast<std::uint8_t>((maxFrameBodyBytes + 1) >> 14));
    link.host().receiveStream(oversized, link.now());
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::Oversize);
}

// A flood of calls is not held: with the owner's busy answerer installed, a
// call past the outstanding-call cap is answered where it is read, so the
// owner is handed no more calls than may be outstanding, each of the rest
// has its busy answer under its own id, and the connection stays.
TEST(callsBeyondThoseOutstandingAreAnsweredBusyAsTheyAreRead)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    answerBusyAsRead(link.host());
    constexpr int flood = 1'000;
    for (int call = 1; call <= flood; ++call) {
        std::vector<std::uint8_t> body;
        encode(body, Call { .text = captureRequest(call) });
        link.sendToHost(FrameKind::Call, body);
    }
    CHECK_MSG(link.host().state() == ConnectionState::Attached, "a thousand calls in one read closed the connection");
    const std::vector<ReceivedCall> held = link.host().takeCalls();
    CHECK_EQ(held.size(), std::size_t { policy.limits.maxOutstandingCalls });
    CHECK(std::ranges::all_of(held, [](const ReceivedCall& call) { return call.admission == CallAdmission::Admitted; }));
    CHECK_EQ(link.host().counters().callsAnsweredBusy, std::uint64_t { flood } - policy.limits.maxOutstandingCalls);
    CHECK(link.host().takeCalls().empty());
    // Every busy answer reaches the client, each naming a call it made.
    link.stepUntilQuiet();
    const std::vector<std::string> results = link.client().takeResults();
    CHECK_EQ(results.size(), static_cast<std::size_t>(flood) - policy.limits.maxOutstandingCalls);
    CHECK(std::ranges::all_of(results, [](const std::string& result) { return errorCode(result) == -32005; }));
    std::set<int> answered;
    for (const std::string& result : results)
        answered.insert(callID(result));
    CHECK_EQ(answered.size(), results.size());
    CHECK(!answered.contains(1) && answered.contains(flood));
}

// A Notify is the host's and draws no answer. A client skips a kind it does
// not know by the frame's length, holds at most `maxUnreadNotifies` unread,
// and drops past it.
TEST(aClientSkipsANotifyKindItDoesNotKnowAndBoundsTheRest)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    const ConsentResolution resolution { .kind = ConsentKind::Attach, .requestID = 1, .sessionID = 2 };
    const auto resolved = consentResolvedNotify(resolution);
    REQUIRE(resolved);
    // Kind 127 with a record, and kind 256, wider than this build's byte.
    const std::vector<std::uint8_t> unknown { 0x7F, 1, 2, 3 };
    const std::vector<std::uint8_t> wider { 0x80, 0x02, 9 };
    CHECK(link.host().sendNotify(unknown));
    CHECK(link.host().sendNotify(*resolved));
    CHECK(link.host().sendNotify(wider));
    CHECK(!link.host().sendNotify({}));
    link.stepUntilQuiet();
    CHECK(link.client().state() == ConnectionState::Attached);
    const auto heard = link.client().takeNotifies();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].kind == NotifyKind::ConsentResolved);
    CHECK(decodeConsentResolved(heard[0].record) == resolution);
    CHECK_EQ(link.client().counters().notifiesSkipped, std::uint64_t { 2 });
    for (std::size_t index = 0; index < maxUnreadNotifies + 8; ++index) {
        REQUIRE(link.host().sendNotify(*resolved));
        if (index % 64 == 63)
            link.stepUntilQuiet();
    }
    link.stepUntilQuiet();
    CHECK_EQ(link.client().takeNotifies().size(), maxUnreadNotifies);
    CHECK_EQ(link.client().counters().notifiesDropped, std::uint64_t { 8 });
    CHECK(link.client().takeResults().empty());
}

// A Notify with no canonical kind is malformed, as is a known kind whose
// record does not read where the connection itself reads it.
TEST(aNotifyWithoutAKindCloses)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Notify, 0, std::vector<std::uint8_t> { 0x80 }));
    link.deliverToClient(frame);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);
}

// A Result from the client answers no call the host made, and a Call or
// Result body that is not UTF-8 is malformed.
TEST(aResultFromTheClientCloses)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint8_t> body;
    encode(body, Result { .transfer = 0, .text = "{}" });
    link.sendToHost(FrameKind::Result, body);
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::Malformed);

    // The table says what each end enforces, so a peer written from it is
    // never closed for following it: a client calls, a host answers and
    // notifies.
    CHECK(directionOf(FrameKind::Call) == Direction::ClientToHost);
    CHECK(directionOf(FrameKind::Result) == Direction::HostToClient);
    CHECK(directionOf(FrameKind::Notify) == Direction::HostToClient);
    SyncLink called(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint8_t> request;
    encode(request, Call { .text = R"({"jsonrpc":"2.0","id":1,"method":"describe"})" });
    std::vector<std::uint8_t> callFrame;
    CHECK(appendFrame(callFrame, FrameKind::Call, 0, request));
    called.deliverToClient(callFrame);
    CHECK(called.client().state() == ConnectionState::Closed);
    CHECK(called.client().byeReason() == ByeReason::Malformed);
    SyncLink notified(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint8_t> notice;
    encode(notice, Notify { .kind = NotifyKind::PasteReady, .record = {} });
    std::vector<std::uint8_t> noticeFrame;
    CHECK(appendFrame(noticeFrame, FrameKind::Notify, 0, notice));
    notified.deliverToClient(noticeFrame);
    CHECK(notified.client().state() == ConnectionState::Attached);
    notified.sendToHost(FrameKind::Notify, notice);
    CHECK(notified.host().state() == ConnectionState::Closed);
    CHECK(notified.host().byeReason() == ByeReason::Malformed);
    std::vector<std::uint8_t> onAPane;
    CHECK(!appendFrame(onAPane, FrameKind::Call, 1, body));
    CHECK(!decodeCall(std::vector<std::uint8_t> { 0xFF, 0xFE }).has_value());
    CHECK(!decodeResult(std::vector<std::uint8_t> { 1, '{', '}' }).has_value());
    const std::optional<Result> announced = decodeResult(std::vector<std::uint8_t> { 5 });
    REQUIRE(announced.has_value());
    CHECK_EQ(announced->transfer, std::uint32_t { 5 });
}

UBIQUITTY_TEST_MAIN()
