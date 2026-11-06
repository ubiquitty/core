// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Control/PasteUpload.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/FrameGates.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Control;
using Codec::JSON::Value;

namespace {

constexpr std::array<std::uint8_t, 8> payload { 0, 1, 2, 3, 255, 254, 128, 0 };

PastePushRequest manifest(std::span<const std::uint8_t> bytes = payload)
{
    PastePushRequest request { .pane = 7, .mime = "image/png", .bytes = static_cast<std::uint32_t>(bytes.size()) };
    Crypto::sha256(bytes, request.sha256);
    return request;
}

Value decoded(std::string_view text)
{
    bool isJSON = false;
    auto value = Codec::JSON::parse(std::string(text), isJSON);
    CHECK(isJSON);
    return value;
}

Value replacing(const Value& object, std::string_view key, Value value)
{
    auto result = Value::object();
    for (const auto& [name, field] : object.members()) {
        if (name != key)
            result.set(name, field);
    }
    result.set(std::string(key), std::move(value));
    return result;
}

Value parameters()
{
    return *decoded(pastePushRequest(manifest(), "paste.7")).find("params");
}

Value finalAnswer(PastePushStatus status = PastePushStatus::Landed)
{
    auto result = Value::object();
    result.set("pane", Value::number(7));
    result.set("status", Value::string(std::string(pastePushStatusName(status))));
    result.set("path", Value::string("/tmp/paste.png"));
    const auto id = Value::string("paste.7");
    return rpcResult(&id, std::move(result));
}

TEST(pasteManifestsAreExactBoundedCallsWithoutInlinePayloadCopies)
{
    for (const std::string& mime : { std::string {}, std::string("application/x-private"), std::string("image/png"), std::string("text/\xCE\xBB") }) {
        auto request = manifest();
        request.mime = mime;
        const auto text = pastePushRequest(request, "paste.7");
        const auto call = decoded(text);
        CHECK(call.stringOr("method") == "paste.push");
        CHECK(call.stringOr("id") == "paste.7");
        REQUIRE(call.find("params"));
        CHECK(pastePushParameters(*call.find("params")) == request);
        CHECK(!call.find("params")->find("data"));
        CHECK(text.size() < 300);
    }
    CHECK(pastePushRequest(manifest(), "").empty());
    CHECK(pastePushRequest(manifest(), std::string(Protocol::Sync::maxFrameBodyBytes + 1, 'x')).empty());
    CHECK(pastePushRequest(manifest(), std::string(Protocol::Sync::maxFrameBodyBytes / 6, '\x01')).empty());
    auto request = manifest();
    request.mime.assign(maxPasteMimeBytes + 1, 'x');
    CHECK(pastePushRequest(request, "paste.7").empty());
    request.mime = std::string("\xFF", 1);
    CHECK(pastePushRequest(request, "paste.7").empty());
}

TEST(pasteManifestsRefuseAmbiguousCountsDigestsAndDuplicateFields)
{
    for (const double bytes : { -1.0, 0.0, 1.5, static_cast<double>(maxPastePushBytes + 1), std::numeric_limits<double>::infinity() })
        CHECK(!pastePushParameters(replacing(parameters(), "bytes", Value::number(bytes))));
    for (const double pane : { -1.0, 0.0, 1.5, 4294967296.0 })
        CHECK(!pastePushParameters(replacing(parameters(), "pane", Value::number(pane))));
    for (const std::string& digest : { std::string {}, std::string(63, '0'), std::string(65, '0'), std::string(64, 'A'), std::string(64, 'g') })
        CHECK(!pastePushParameters(replacing(parameters(), "sha256", Value::string(digest))));
    CHECK(!pastePushParameters(replacing(parameters(), "bytes", Value::string("8"))));
    CHECK(!pastePushParameters(replacing(parameters(), "mime", Value::string(std::string(maxPasteMimeBytes + 1, 'x')))));
    auto repeated = parameters();
    repeated.set("bytes", Value::number(8));
    CHECK(!pastePushParameters(repeated));
    repeated = parameters();
    repeated.set("extra", Value::null());
    CHECK(!pastePushParameters(repeated));
}

TEST(aViewerIsRefusedPanePasteBeforeItsManifestIsExamined)
{
    const auto* method = findSyncMethod("paste.push");
    REQUIRE(method);
    CHECK(method->scope == Protocol::Sync::Scope::Drive);
    CHECK(method->target == CallTarget::Pane);
    CallerAuthority caller { .role = Protocol::Sync::Role::Viewer, .scopes = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive }, .origin = CapabilityOrigin::Remote };
    const auto refusedViewer = admitCall(method, Value::object(), caller);
    REQUIRE(refusedViewer);
    CHECK(refusedViewer->code == ErrorCode::Forbidden);
    caller.role = Protocol::Sync::Role::Owner;
    caller.scopes = { Protocol::Sync::Scope::Observe };
    const auto refusedScope = admitCall(method, parameters(), caller);
    REQUIRE(refusedScope);
    CHECK(refusedScope->code == ErrorCode::Forbidden);
    caller.scopes.add(Protocol::Sync::Scope::Drive);
    CHECK(!admitCall(method, parameters(), caller));
    caller.role = Protocol::Sync::Role::Agent;
    CHECK(!admitCall(method, parameters(), caller));
}

TEST(aDriveScopedAgentCanSendOnlyItsCreditedAndStillAuthorizedPasteChunks)
{
    using namespace Protocol::Sync;
    const auto* method = findSyncMethod("paste.push");
    REQUIRE(method);
    CallerAuthority caller { .role = Role::Agent, .scopes = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive }, .origin = CapabilityOrigin::Remote };
    CHECK(!admitCall(method, parameters(), caller));
    REQUIRE(admitsSender(FrameKind::BulkChunk, caller.role));
    CHECK(!admitsSender(FrameKind::BulkChunk, Role::Viewer));
    const auto request = manifest();
    PasteUpload upload;
    REQUIRE(upload.begin(request, 9, [&](std::size_t) { return !admitCall(method, parameters(), caller); }));
    REQUIRE(upload.grantCredit(9));
    const BulkChunk chunk { .transfer = 9, .index = 0, .isLast = true, .bytes = payload, .sha256 = request.sha256 };
    CHECK(upload.accept(chunk, [&] { return !admitCall(method, parameters(), caller); }) == PasteUploadProgress::Complete);
    CHECK(std::ranges::equal(upload.data(), payload));
    upload.clear();
    REQUIRE(upload.begin(request, 9, [&](std::size_t) { return !admitCall(method, parameters(), caller); }));
    // Frame permission alone cannot turn a guessed reservation into credit.
    CHECK(upload.accept(chunk, [&] { return !admitCall(method, parameters(), caller); }) == PasteUploadProgress::Refused);
    CHECK(upload.heldBytes() == 0);
    REQUIRE(upload.begin(request, 9, [&](std::size_t) { return !admitCall(method, parameters(), caller); }));
    REQUIRE(upload.grantCredit(9));
    caller.scopes = { Protocol::Sync::Scope::Observe };
    CHECK(upload.accept(chunk, [&] { return !admitCall(method, parameters(), caller); }) == PasteUploadProgress::Refused);
    CHECK(upload.heldBytes() == 0 && upload.data().empty());
}

TEST(pasteCreditNamesOneExactCallPaneAndNonzeroTransfer)
{
    const PasteReady ready { "paste.7", 7, 9 };
    const auto body = pasteReadyNotify(ready);
    REQUIRE(body);
    const auto head = Protocol::Sync::decodeNotifyHead(*body);
    REQUIRE(head && head->kind == std::to_underlying(Protocol::Sync::NotifyKind::PasteReady));
    CHECK(decodePasteReady(head->record) == ready);
    CHECK(!pasteReadyNotify({ "", 7, 9 }));
    CHECK(!pasteReadyNotify({ "paste.7", 0, 9 }));
    CHECK(!pasteReadyNotify({ "paste.7", 7, 0 }));
    CHECK(!pasteReadyNotify({ std::string("paste\0", 6), 7, 9 }));
    CHECK(!pasteReadyNotify({ std::string(Protocol::Sync::maxFrameBodyBytes, 'x'), 7, 9 }));
    const std::vector<std::uint8_t> record(head->record.begin(), head->record.end());
    for (std::size_t cut = 0; cut < record.size(); ++cut)
        CHECK(!decodePasteReady(std::span(record).first(cut)));
    auto extended = record;
    extended.push_back(0);
    CHECK(!decodePasteReady(extended));
    auto zeroTransfer = record;
    std::fill(zeroTransfer.end() - 4, zeroTransfer.end(), std::uint8_t { 0 });
    CHECK(!decodePasteReady(zeroTransfer));
}

TEST(pasteFinalResultsPreserveEveryStatusButNeverInferSuccessFromCredit)
{
    for (const auto status : { PastePushStatus::Landed, PastePushStatus::TooLarge, PastePushStatus::IOError, PastePushStatus::Unsupported, PastePushStatus::Busy }) {
        const auto answer = decodePastePushReply(finalAnswer(status), "paste.7", 7);
        REQUIRE(answer);
        CHECK(answer->status == status);
        CHECK(answer->path == "/tmp/paste.png");
    }
    CHECK(pastePushStatusName(static_cast<PastePushStatus>(5)).empty());
    const auto unrelated = decodePastePushReply(finalAnswer(), "paste.8", 7);
    CHECK(!unrelated && unrelated.error() == PasteReplyFailure::Unrelated);
    CHECK(!decodePastePushReply(finalAnswer(), "paste.7", 8));
    auto changed = finalAnswer();
    changed = replacing(changed, "result", replacing(*changed.find("result"), "status", Value::string("done")));
    CHECK(!decodePastePushReply(changed, "paste.7", 7));
    changed = finalAnswer();
    changed = replacing(changed, "result", replacing(*changed.find("result"), "path", Value::string(std::string(maxPastePathBytes + 1, 'x'))));
    CHECK(!decodePastePushReply(changed, "paste.7", 7));
    changed = finalAnswer();
    changed.set("id", Value::string("paste.7"));
    CHECK(!decodePastePushReply(changed, "paste.7", 7));
    const auto id = Value::string("paste.7");
    const auto error = decodePastePushReply(rpcError(&id, ErrorCode::Forbidden, "no Drive authority"), "paste.7", 7);
    CHECK(!error && error.error() == PasteReplyFailure::Refused);
}

TEST(aPasteUploadOwnsNothingUntilAuthorityAndBothBudgetsAdmitIt)
{
    PasteUpload upload;
    unsigned queries = 0;
    const auto denied = [&](std::size_t bytes) {
        ++queries;
        CHECK_EQ(bytes, payload.size());
        CHECK_EQ(upload.heldBytes(), 0u);
        return false;
    };
    CHECK(!upload.begin(manifest(), 9, denied));
    CHECK_EQ(queries, 1u);
    CHECK(!upload.isActive());
    CHECK_EQ(upload.heldBytes(), 0u);
    auto invalid = manifest();
    invalid.bytes = maxPastePushBytes + 1;
    CHECK(!upload.begin(invalid, 9, denied));
    CHECK(!upload.begin(manifest(), 0, denied));
    CHECK_EQ(queries, 1u);
    CHECK(upload.begin(manifest(), 9, [](std::size_t bytes) { return bytes <= payload.size(); }));
    CHECK(upload.isActive());
    CHECK_EQ(upload.heldBytes(), payload.size());
    CHECK(upload.data().empty());
    CHECK(!upload.begin(manifest(), 10, denied));
    CHECK_EQ(queries, 1u);
    CHECK_EQ(upload.transfer(), 9u);
    upload.clear();
    CHECK(!upload.isActive());
    CHECK_EQ(upload.heldBytes(), 0u);
}

TEST(aPasteUploadExposesOnlyTheCompleteDeclaredAndVerifiedBody)
{
    PasteUpload upload;
    const auto request = manifest();
    REQUIRE(upload.begin(request, 9, [](std::size_t) { return true; }));
    REQUIRE(upload.grantCredit(9));
    const Protocol::Sync::BulkChunk first { .transfer = 9, .index = 0, .bytes = std::span(payload).first(3) };
    CHECK(upload.accept(first, [] { return true; }) == PasteUploadProgress::More);
    CHECK(upload.data().empty());
    CHECK_EQ(upload.heldBytes(), payload.size());
    const Protocol::Sync::BulkChunk last { .transfer = 9, .index = 1, .isLast = true, .bytes = std::span(payload).subspan(3), .sha256 = request.sha256 };
    CHECK(upload.accept(last, [] { return true; }) == PasteUploadProgress::Complete);
    CHECK(upload.isComplete());
    CHECK(std::ranges::equal(upload.data(), payload));
    CHECK_EQ(upload.heldBytes(), payload.size());
    upload.clear();
    CHECK(upload.data().empty());
    CHECK_EQ(upload.heldBytes(), 0u);
}

TEST(aPasteUploadRefusesOrphansReorderingOverflowCompressionAndBadFinalHashes)
{
    const auto request = manifest();
    const Protocol::Sync::BulkChunk complete { .transfer = 9, .isLast = true, .bytes = payload, .sha256 = request.sha256 };
    PasteUpload orphan;
    CHECK(orphan.accept(complete, [] { return true; }) == PasteUploadProgress::Refused);
    const std::vector<std::uint8_t> oversized(Protocol::Sync::bulkChunkPayloadBytes + 1, 0);
    for (unsigned fault = 0; fault < 10; ++fault) {
        PasteUpload upload;
        REQUIRE(upload.begin(request, 9, [](std::size_t) { return true; }));
        REQUIRE(upload.grantCredit(9));
        auto chunk = complete;
        switch (fault) {
        case 0:
            chunk.transfer = 10;
            break;
        case 1:
            chunk.index = 1;
            break;
        case 2:
            chunk.bytes = {};
            break;
        case 3:
            chunk.bytes = oversized;
            break;
        case 4:
            chunk.compression = Protocol::Sync::Compression::Deflate;
            break;
        case 5:
            chunk.inflatedBytes = payload.size();
            break;
        case 6:
            chunk.bytes = std::span(payload).first(3);
            break;
        case 7:
            chunk.isLast = false;
            break;
        case 8:
            chunk.sha256[0] ^= 1;
            break;
        case 9:
            chunk.bytes = std::span(oversized).first(payload.size());
            break;
        }
        CHECK(upload.accept(chunk, [] { return true; }) == PasteUploadProgress::Refused);
        CHECK(!upload.isActive());
        CHECK(upload.data().empty());
        CHECK_EQ(upload.heldBytes(), 0u);
    }
}

TEST(aPasteUploadRechecksCurrentAuthorityBeforeEveryChunk)
{
    PasteUpload upload;
    const auto request = manifest();
    REQUIRE(upload.begin(request, 9, [](std::size_t) { return true; }));
    REQUIRE(upload.grantCredit(9));
    const Protocol::Sync::BulkChunk chunk { .transfer = 9, .isLast = true, .bytes = payload, .sha256 = request.sha256 };
    unsigned queries = 0;
    CHECK(upload.accept(chunk, [&] { ++queries; return false; }) == PasteUploadProgress::Refused);
    CHECK_EQ(queries, 1u);
    CHECK(!upload.isActive());
    CHECK(upload.data().empty());
    CHECK_EQ(upload.heldBytes(), 0u);
}

TEST(aFullSizePasteUsesOnlyBoundedRawChunksWithoutShrinkingItsExistingLimit)
{
    const std::vector<std::uint8_t> bytes(maxPastePushBytes, 0xA5);
    const auto request = manifest(bytes);
    CHECK(pastePushRequest(request, "paste.large").size() < 300);
    PasteUpload upload;
    REQUIRE(upload.begin(request, 9, [](std::size_t amount) { return amount == maxPastePushBytes; }));
    REQUIRE(upload.grantCredit(9));
    std::size_t offset = 0;
    std::uint32_t index = 0;
    while (offset < bytes.size()) {
        const auto length = std::min(Protocol::Sync::bulkChunkPayloadBytes, bytes.size() - offset);
        const bool isLast = offset + length == bytes.size();
        const Protocol::Sync::BulkChunk chunk { .transfer = 9, .index = index++, .isLast = isLast, .bytes = std::span(bytes).subspan(offset, length), .sha256 = request.sha256 };
        CHECK(upload.accept(chunk, [] { return true; }) == (isLast ? PasteUploadProgress::Complete : PasteUploadProgress::More));
        CHECK_EQ(upload.heldBytes(), maxPastePushBytes);
        offset += length;
    }
    CHECK(index > 1);
    CHECK(std::ranges::equal(upload.data(), bytes));
}

TEST(anOutgoingPasteWaitsForExactCreditAndNeverRewindsOnItsRepeat)
{
    PasteUploadSource source;
    unsigned admissionQueries = 0;
    CHECK(!source.begin(7, "image/png", "paste.7", payload, [&](std::size_t) { ++admissionQueries; return false; }));
    CHECK_EQ(admissionQueries, 1u);
    CHECK(!source.isActive());
    REQUIRE(source.begin(7, "image/png", "paste.7", payload, [](std::size_t bytes) { return bytes == payload.size() + PasteUploadSource::metadataReservationBytes; }));
    CHECK(source.retainedBytes() <= payload.size() + PasteUploadSource::metadataReservationBytes);
    unsigned sends = 0;
    const auto send = [&](const Protocol::Sync::BulkChunk&) { ++sends; return true; };
    CHECK(!source.sendOne(send));
    CHECK_EQ(sends, 0u);
    CHECK(source.credit({ "paste.other", 7, 9 }) == PasteCreditDisposition::Unrelated);
    CHECK(source.credit({ "paste.7", 8, 9 }) == PasteCreditDisposition::Unrelated);
    CHECK(source.credit({ "paste.7", 7, 0 }) == PasteCreditDisposition::Refused);
    CHECK(source.credit({ "paste.7", 7, 9 }) == PasteCreditDisposition::Accepted);
    CHECK(!source.sendOne([](const Protocol::Sync::BulkChunk&) { return false; }));
    CHECK_EQ(source.queuedBytes(), 0u);
    CHECK(source.credit({ "paste.7", 7, 9 }) == PasteCreditDisposition::Repeated);
    CHECK(source.sendOne([&](const Protocol::Sync::BulkChunk& chunk) {
        CHECK_EQ(chunk.index, 0u);
        CHECK(chunk.isLast);
        CHECK(std::ranges::equal(chunk.bytes, payload));
        return true;
    }));
    CHECK_EQ(source.queuedBytes(), payload.size());
    CHECK(source.isActive()); // The exact final Result, not credit/queueing, settles it.
    CHECK(!source.hasBytesToSend());
    CHECK(source.credit({ "paste.7", 7, 9 }) == PasteCreditDisposition::Repeated);
    CHECK(source.credit({ "paste.7", 7, 10 }) == PasteCreditDisposition::Refused);
    CHECK(!source.sendOne(send));
    CHECK_EQ(sends, 0u);
    source.clear();
    CHECK_EQ(source.heldBytes(), 0u);
    CHECK(source.credit({ "paste.7", 7, 9 }) == PasteCreditDisposition::Unrelated);
}

TEST(anOutgoingPasteCapturesItsImmutableBytesBeforeTheBorrowReturns)
{
    std::vector<std::uint8_t> borrowed(payload.begin(), payload.end());
    PasteUploadSource source;
    REQUIRE(source.begin(7, "", "paste.7", borrowed, [](std::size_t) { return true; }));
    std::fill(borrowed.begin(), borrowed.end(), 0);
    const auto call = decoded(source.call());
    REQUIRE(call.find("params"));
    const auto captured = pastePushParameters(*call.find("params"));
    REQUIRE(captured);
    CHECK(captured->sha256 == manifest().sha256);
    CHECK(captured->mime.empty());
    REQUIRE(source.credit({ "paste.7", 7, 9 }) == PasteCreditDisposition::Accepted);
    CHECK(source.sendOne([&](const Protocol::Sync::BulkChunk& chunk) {
        CHECK(std::ranges::equal(chunk.bytes, payload));
        CHECK(chunk.sha256 == manifest().sha256);
        return true;
    }));
}

TEST(aFullSizeOutgoingPasteAndCreditedReceiverAgreeOnEveryBoundedChunk)
{
    const std::vector<std::uint8_t> bytes(maxPastePushBytes, 0x3C);
    PasteUploadSource source;
    REQUIRE(source.begin(7, "image/png", "paste.large", bytes, [](std::size_t amount) { return amount == maxPastePushBytes + PasteUploadSource::metadataReservationBytes; }));
    CHECK(source.retainedBytes() <= maxPastePushBytes + PasteUploadSource::metadataReservationBytes);
    PasteUpload receiver;
    REQUIRE(receiver.begin(source.request(), 9, [](std::size_t amount) { return amount == maxPastePushBytes; }));
    REQUIRE(receiver.grantCredit(9));
    REQUIRE(source.credit({ "paste.large", 7, 9 }) == PasteCreditDisposition::Accepted);
    std::uint32_t nextIndex = 0;
    while (source.hasBytesToSend()) {
        CHECK(source.credit({ "paste.large", 7, 9 }) == PasteCreditDisposition::Repeated);
        REQUIRE(source.sendOne([&](const Protocol::Sync::BulkChunk& chunk) {
            CHECK_EQ(chunk.index, nextIndex++);
            CHECK(chunk.bytes.size() <= Protocol::Sync::bulkChunkPayloadBytes);
            const auto progress = receiver.accept(chunk, [] { return true; });
            CHECK(progress == (chunk.isLast ? PasteUploadProgress::Complete : PasteUploadProgress::More));
            return progress != PasteUploadProgress::Refused;
        }));
    }
    CHECK(nextIndex > 1);
    CHECK_EQ(source.queuedBytes(), maxPastePushBytes);
    CHECK_EQ(source.heldBytes(), maxPastePushBytes);
    CHECK(std::ranges::equal(receiver.data(), bytes));
    source.clear();
    receiver.clear();
    CHECK_EQ(source.heldBytes() + receiver.heldBytes(), 0u);
}

TEST(aPasteUploadRefusesGuessingItsReservedTransferBeforeCreditWasAdmitted)
{
    const auto request = manifest();
    const Protocol::Sync::BulkChunk chunk { .transfer = 9, .isLast = true, .bytes = payload, .sha256 = request.sha256 };
    PasteUpload notCredited;
    REQUIRE(notCredited.begin(request, 9, [](std::size_t) { return true; }));
    CHECK(notCredited.accept(chunk, [] { return true; }) == PasteUploadProgress::Refused);
    CHECK_EQ(notCredited.heldBytes(), 0u);
    PasteUpload wrongCredit;
    REQUIRE(wrongCredit.begin(request, 9, [](std::size_t) { return true; }));
    CHECK(!wrongCredit.grantCredit(10));
    CHECK(wrongCredit.accept(chunk, [] { return true; }) == PasteUploadProgress::Refused);
    PasteUpload credited;
    REQUIRE(credited.begin(request, 9, [](std::size_t) { return true; }));
    REQUIRE(credited.grantCredit(9));
    CHECK(credited.grantCredit(9));
    CHECK(credited.accept(chunk, [] { return true; }) == PasteUploadProgress::Complete);
    CHECK(std::ranges::equal(credited.data(), payload));
}

} // namespace

UBIQUITTY_TEST_MAIN()
