// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/ReclaimWire.h"
#include "Core/Session/ReclaimTransactionWire.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Session;
namespace Local = Protocol::Local;
namespace Sync = Protocol::Sync;

constexpr AdoptOperation adoption { { 151, 161 }, 17 };
constexpr AdoptOperation operation { { 51, 61 }, 17 };
constexpr ReclaimInputBarrier barrier { { 71, 81 }, 12 };
constexpr Protocol::Control::ReclaimTarget target {
    .handle = 3,
    .sessionID = 11,
    .daemonInstanceID = 17,
    .hostInstanceID = { 21, 31 },
    .attachmentID = { 41, 51 },
    .paneID = { 11, 21 },
};
constexpr ReclaimTransactionSelector selector { adoption, operation, target, barrier };
constexpr std::uint64_t correlation = 0x3837363534333231;
constexpr std::array<std::uint8_t, 7> opaqueTerminal { 0, 0xFF, 0x1B, 7, 0, 0x80, 1 };
constexpr std::array<char, 8> suffix { 'd', 'e', 'f', 'r', 'e', 'p', 'l', 'y' };
constexpr std::array<PAL::OutputQueue::Run, 2> runs { { { 81, 3 }, { 0, 5 } } };
constexpr Local::ReclaimWireLimits wireLimits { 5 * 1024 * 1024, 4 * 1024 * 1024, 4096, 128 };

// Explicit small policy; no Preferences configuration, socket, stream, process,
// default host policy or system clock is constructed by this fixture.
constexpr Sync::HandoverTerminalLimits terminalLimits {
    .maxRecordBytes = 4 * 1024 * 1024,
    .maxCheckpointBytes = 1024 * 1024,
    .maxScreenCellBytes = 64 * 1024,
    .maxHostStateBytes = 256 * 1024,
    .maxLinkBytes = 64 * 1024,
    .maxParserBytes = 4096,
    .images = { 8192, Image::terminalImageBytesBudgetDefault },
    .history = { 256 * 1024, 256, 256, 16384 },
    .text = { 64 * 1024, ClusterPool::maxEntries, RunTextStore::maxEntries, 16384 },
    .visual = { 64 * 1024, Tek::tekMaxPrimitives },
    .sounds = { 64 * 1024, 0 },
};
constexpr ReclaimTransferLimits transferLimits { terminalLimits, 4096, 5 * 1024 * 1024 };

ReclaimCapsuleView capsule(std::span<const std::uint8_t> body = opaqueTerminal)
{
    ReclaimCapsuleView value { .target = target, .operation = operation, .barrier = barrier, .inputWritten = barrier.finalInputSeq, .terminalBody = body, .acceptedPTY = suffix, .acceptedRuns = runs };
    const auto digest = reclaimCapsuleSHA256(value, transferLimits);
    CHECK(digest.has_value());
    if (digest)
        value.capsuleSHA256 = *digest;
    return value;
}

Local::ReclaimWireSelector wireSelector()
{
    const auto value = reclaimSelectorToWire(selector);
    CHECK(value.has_value());
    return value.value_or(Local::ReclaimWireSelector {});
}

Local::ReclaimWireCapsule wireCapsule(std::span<Local::ReclaimWireRun> runStorage)
{
    const auto value = reclaimCapsuleToWire(adoption, correlation, capsule(), transferLimits, runStorage);
    CHECK(value.has_value());
    return value.value_or(Local::ReclaimWireCapsule {});
}

Local::ReclaimWireOutcome wireOutcome(AdoptDisposition disposition = AdoptDisposition::Committed)
{
    ReclaimTransferEvidence evidence { .target = target, .barrier = barrier, .capsuleSHA256 = capsule().capsuleSHA256, .outcome = { .operation = operation, .disposition = disposition } };
    if (disposition != AdoptDisposition::Unknown)
        evidence.outcome.target = { target.sessionID, target.paneID };
    if (disposition == AdoptDisposition::Rejected)
        evidence.outcome.rejection = AdoptRejection::InvalidCandidate;
    const auto value = reclaimOutcomeToWire(adoption, evidence, correlation);
    CHECK(value.has_value());
    return value.value_or(Local::ReclaimWireOutcome {});
}

std::vector<std::uint8_t> record(const Local::EncodedReclaimWireCapsule& encoded)
{
    std::vector<std::uint8_t> bytes(encoded.prefix().begin(), encoded.prefix().end());
    bytes.insert(bytes.end(), encoded.terminalBody.begin(), encoded.terminalBody.end());
    bytes.insert(bytes.end(), encoded.acceptedPTY.begin(), encoded.acceptedPTY.end());
    return bytes;
}

void set64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
}

void set32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
}

std::vector<Local::ReclaimWireSelector> changedSelectors()
{
    std::vector<Local::ReclaimWireSelector> changed;
    const auto add = [&](auto change) {
        auto value = wireSelector();
        change(value);
        changed.push_back(value);
    };
    add([](auto& value) { ++value.adoption.operationID.low; });
    add([](auto& value) { ++value.adoption.operationID.high; });
    add([](auto& value) { ++value.operation.operationID.low; });
    add([](auto& value) { ++value.operation.operationID.high; });
    add([](auto& value) { ++value.target.handle; });
    add([](auto& value) { ++value.target.sessionID; });
    add([](auto& value) { ++value.target.hostInstanceID.low; });
    add([](auto& value) { ++value.target.hostInstanceID.high; });
    add([](auto& value) { ++value.target.attachmentID.low; });
    add([](auto& value) { ++value.target.attachmentID.high; });
    add([](auto& value) { ++value.target.paneID.low; });
    add([](auto& value) { ++value.target.paneID.high; });
    add([](auto& value) { ++value.barrier.clientInstance.low; });
    add([](auto& value) { ++value.barrier.clientInstance.high; });
    add([](auto& value) { ++value.barrier.finalInputSeq; });
    add([](auto& value) {
        ++value.adoption.daemonInstanceID;
        ++value.operation.daemonInstanceID;
        ++value.target.daemonInstanceID;
    });
    return changed;
}

} // namespace

TEST(reclaimWireReadySelectsOnlyOperationFiveAndPinsCorrelationAndDaemon)
{
    for (const auto status : { Local::ReclaimWireReadyStatus::Ready, Local::ReclaimWireReadyStatus::Refused }) {
        const Local::ReclaimWireReady ready { correlation, operation.daemonInstanceID, status };
        const auto encoded = Local::encodeLocalReclaimReady(ready);
        REQUIRE(encoded);
        CHECK_EQ((*encoded)[Local::localControlPreamble.size()], 0x85u);
        const auto decoded = Local::decodeLocalReclaimReady(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == ready);
        for (std::size_t size = 0; size < encoded->size(); ++size)
            CHECK(!Local::decodeLocalReclaimReady(std::span(*encoded).first(size)));
        auto changed = *encoded;
        changed[Local::localControlPreamble.size()] = Local::localHandoffReplyOperation;
        CHECK(!Local::decodeLocalReclaimReady(changed));
        changed = *encoded;
        changed[Local::localControlPreamble.size() + 1] = 2;
        CHECK(!Local::decodeLocalReclaimReady(changed));
    }
    CHECK(!Local::encodeLocalReclaimReady({ 0, 17, Local::ReclaimWireReadyStatus::Ready }));
    CHECK(!Local::encodeLocalReclaimReady({ 1, 0, Local::ReclaimWireReadyStatus::Ready }));
}

TEST(reclaimWireCommandsPreserveEverySelectorAndCanonicalOptionalDigest)
{
    static_assert(Local::reclaimWireCommandBytes == 182);
    const auto digest = capsule().capsuleSHA256;
    for (const auto kind : { Local::ReclaimWireMessage::Prepare, Local::ReclaimWireMessage::Commit,
             Local::ReclaimWireMessage::Abort, Local::ReclaimWireMessage::Status }) {
        Local::ReclaimWireCommand command { .kind = kind, .correlation = correlation, .selector = wireSelector() };
        if (kind != Local::ReclaimWireMessage::Prepare)
            command.capsuleSHA256 = digest;
        const auto encoded = Local::encodeReclaimWireCommand(command);
        REQUIRE(encoded);
        CHECK_EQ((*encoded)[0], std::to_underlying(kind));
        CHECK_EQ((*encoded)[1], 177u);
        CHECK_EQ((*encoded)[5], 0x31u);
        const auto decoded = Local::decodeReclaimWireCommand(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == command);
        for (std::size_t size = 0; size < encoded->size(); ++size)
            CHECK(!Local::decodeReclaimWireCommand(std::span(*encoded).first(size)));
        auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
        trailing.push_back(0);
        CHECK(!Local::decodeReclaimWireCommand(trailing));
    }
    CHECK(!Local::encodeReclaimWireCommand({ Local::ReclaimWireMessage::Prepare, correlation, wireSelector(), digest }));
    CHECK(!Local::encodeReclaimWireCommand({ Local::ReclaimWireMessage::Commit, correlation, wireSelector(), std::nullopt }));
    for (const auto kind : { Local::ReclaimWireMessage::Abort, Local::ReclaimWireMessage::Status }) {
        const auto absent = Local::encodeReclaimWireCommand({ kind, correlation, wireSelector(), std::nullopt });
        REQUIRE(absent);
        CHECK(std::ranges::all_of(std::span(*absent).last<33>(), [](auto byte) { return byte == 0; }));
        auto malformed = *absent;
        malformed.back() = 1;
        CHECK(!Local::decodeReclaimWireCommand(malformed));
        malformed = *absent;
        malformed[Local::reclaimWireCommandBytes - 33] = 2;
        CHECK(!Local::decodeReclaimWireCommand(malformed));
    }
    auto sameOperation = wireSelector();
    sameOperation.adoption = sameOperation.operation;
    CHECK(!Local::encodeReclaimWireCommand({ Local::ReclaimWireMessage::Prepare, correlation, sameOperation, std::nullopt }));
}

TEST(reclaimWireCommandsRefuseEveryEmptyIdentityAndInconsistentDaemon)
{
    const auto original = Local::encodeReclaimWireCommand({ Local::ReclaimWireMessage::Prepare, correlation, wireSelector(), std::nullopt });
    REQUIRE(original);
    constexpr std::array<std::pair<std::size_t, std::size_t>, 12> fields { {
        { 5, 8 },
        { 13, 16 },
        { 29, 8 },
        { 37, 16 },
        { 53, 8 },
        { 61, 4 },
        { 65, 4 },
        { 69, 8 },
        { 77, 16 },
        { 93, 16 },
        { 109, 16 },
        { 125, 16 },
    } };
    for (const auto& [offset, size] : fields) {
        auto bytes = *original;
        std::ranges::fill(std::span(bytes).subspan(offset, size), 0);
        CHECK(!Local::decodeReclaimWireCommand(bytes));
    }
    for (const auto offset : { 29u, 53u, 69u }) {
        auto bytes = *original;
        ++bytes[offset];
        CHECK(!Local::decodeReclaimWireCommand(bytes));
    }
}

TEST(reclaimWireHeaderChecksEveryKindWholeRecordBoundAndFixedShape)
{
    for (unsigned value = 0; value <= 255; ++value) {
        const auto kind = static_cast<Local::ReclaimWireMessage>(value);
        const bool isCommand = value >= 1 && value <= 4;
        const bool isKnown = isCommand || kind == Local::ReclaimWireMessage::Capsule
            || kind == Local::ReclaimWireMessage::Outcome || kind == Local::ReclaimWireMessage::Refused;
        const auto length = kind == Local::ReclaimWireMessage::Capsule ? Local::reclaimWireCapsuleHeaderBytes + 1
            : kind == Local::ReclaimWireMessage::Outcome               ? Local::reclaimWireOutcomeBytes
            : kind == Local::ReclaimWireMessage::Refused               ? Local::reclaimWireRefusedBytes
                                                                       : Local::reclaimWireCommandBytes;
        std::array<std::uint8_t, 5> header { static_cast<std::uint8_t>(value) };
        set32(header, 1, static_cast<std::uint32_t>(length - 5));
        CHECK_EQ(Local::decodeReclaimWireHeader(header, length).has_value(), isKnown);
        CHECK(!Local::decodeReclaimWireHeader(header, length - 1));
        if (isKnown) {
            set32(header, 1, static_cast<std::uint32_t>(length - 6));
            CHECK(!Local::decodeReclaimWireHeader(header, length));
        }
    }
    const std::array<std::uint8_t, 5> huge { 0x81, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(!Local::decodeReclaimWireHeader(huge, 4096));
    const auto result = Local::decodeReclaimWireHeader(huge, std::numeric_limits<std::size_t>::max());
    CHECK_EQ(result.has_value(), std::uint64_t { UINT32_MAX } + 5 <= std::numeric_limits<std::size_t>::max());
}

TEST(reclaimWireCapsuleBorrowsTheEntireBinaryTerminalAndAcceptedPartition)
{
    static_assert(Local::reclaimWireCapsuleHeaderBytes == 259);
    static_assert(Local::reclaimWireMaxCapsulePrefixBytes == 2307);
    std::array<Local::ReclaimWireRun, 128> sourceRuns {}, restoredRuns {};
    const auto original = wireCapsule(sourceRuns);
    const auto encoded = Local::encodeReclaimWireCapsule(original, wireLimits);
    REQUIRE(encoded);
    CHECK_EQ(encoded->prefix().size(), Local::reclaimWireCapsuleHeaderBytes + 32);
    CHECK(encoded->terminalBody.data() == opaqueTerminal.data());
    CHECK(encoded->acceptedPTY.data() == reinterpret_cast<const std::uint8_t*>(suffix.data()));
    const auto bytes = record(*encoded);
    const auto decoded = Local::decodeReclaimWireCapsule(bytes, wireLimits, restoredRuns);
    REQUIRE(decoded);
    CHECK(decoded->selector == original.selector);
    CHECK(decoded->prepared == original.prepared);
    CHECK(decoded->inputWritten == original.inputWritten);
    CHECK(decoded->capsuleSHA256 == original.capsuleSHA256);
    CHECK(std::ranges::equal(decoded->acceptedRuns, original.acceptedRuns));
    CHECK(std::ranges::equal(decoded->terminalBody, opaqueTerminal));
    CHECK(decoded->terminalBody.data() == bytes.data() + encoded->prefix().size());
    CHECK(decoded->acceptedPTY.data() == decoded->terminalBody.data() + opaqueTerminal.size());
    CHECK(decoded->acceptedRuns.data() == restoredRuns.data());
    const auto repeated = Local::encodeReclaimWireCapsule(*decoded, wireLimits);
    REQUIRE(repeated);
    CHECK(record(*repeated) == bytes);
}

TEST(reclaimWireCapsuleRejectsTruncationTrailingLengthsPartitionAndIndependentCaps)
{
    std::array<Local::ReclaimWireRun, 128> sourceRuns {}, destination;
    std::ranges::fill(destination, Local::ReclaimWireRun { 999, 999 });
    const auto value = wireCapsule(sourceRuns);
    const auto encoded = Local::encodeReclaimWireCapsule(value, wireLimits);
    REQUIRE(encoded);
    const auto original = record(*encoded);
    for (std::size_t size = 0; size < original.size(); ++size)
        CHECK(!Local::decodeReclaimWireCapsule(std::span(original).first(size), wireLimits, destination));
    auto trailing = original;
    trailing.push_back(0);
    CHECK(!Local::decodeReclaimWireCapsule(trailing, wireLimits, destination));
    set32(trailing, 1, static_cast<std::uint32_t>(trailing.size() - 5));
    CHECK(!Local::decodeReclaimWireCapsule(trailing, wireLimits, destination));
    for (const auto offset : { 182u, 190u }) {
        auto bytes = original;
        set64(bytes, offset, UINT64_MAX);
        CHECK(!Local::decodeReclaimWireCapsule(bytes, wireLimits, destination));
    }
    for (const auto bytesInFirstRun : { std::uint64_t { 0 }, std::uint64_t { 2 }, UINT64_MAX }) {
        auto bytes = original;
        set64(bytes, Local::reclaimWireCapsuleHeaderBytes + 8, bytesInFirstRun);
        CHECK(!Local::decodeReclaimWireCapsule(bytes, wireLimits, destination));
    }
    auto sameOwner = original;
    set64(sameOwner, Local::reclaimWireCapsuleHeaderBytes + 16, 81);
    CHECK(!Local::decodeReclaimWireCapsule(sameOwner, wireLimits, destination));
    auto tooMany = original;
    tooMany[198] = 129;
    CHECK(!Local::decodeReclaimWireCapsule(tooMany, wireLimits, destination));
    for (unsigned field = 0; field < 4; ++field) {
        auto limited = wireLimits;
        if (field == 0)
            limited.maxRecordBytes = original.size() - 1;
        else if (field == 1)
            limited.maxTerminalBytes = opaqueTerminal.size() - 1;
        else if (field == 2)
            limited.maxAcceptedPTYBytes = suffix.size() - 1;
        else
            limited.maxRuns = 1;
        CHECK(!Local::encodeReclaimWireCapsule(value, limited));
        CHECK(!Local::decodeReclaimWireCapsule(original, limited, destination));
    }
    CHECK(!Local::decodeReclaimWireCapsule(original, wireLimits, std::span(destination).first(1)));
    CHECK(std::ranges::all_of(destination, [](const auto& run) { return run == Local::ReclaimWireRun { 999, 999 }; }));
}

TEST(reclaimWireCapsulePreservesEveryMaximumRunAndTheEmptySuffixWithoutTruncation)
{
    std::array<Local::ReclaimWireRun, 129> sourceRuns;
    std::array<Local::ReclaimWireRun, 128> restoredRuns;
    std::array<std::uint8_t, 129> accepted {};
    auto value = wireCapsule(sourceRuns);
    for (std::size_t index = 0; index < sourceRuns.size(); ++index)
        sourceRuns[index] = { .owner = index % 2 ? 0u : 81u, .bytes = 1 };
    value.acceptedRuns = std::span(sourceRuns).first(128);
    value.acceptedPTY = std::span(accepted).first(128);
    const auto encoded = Local::encodeReclaimWireCapsule(value, wireLimits);
    REQUIRE(encoded);
    CHECK_EQ(encoded->prefix().size(), Local::reclaimWireMaxCapsulePrefixBytes);
    const auto bytes = record(*encoded);
    const auto decoded = Local::decodeReclaimWireCapsule(bytes, wireLimits, restoredRuns);
    REQUIRE(decoded);
    CHECK_EQ(decoded->acceptedRuns.size(), 128u);
    CHECK(std::ranges::equal(decoded->acceptedRuns, value.acceptedRuns));
    value.acceptedRuns = sourceRuns;
    value.acceptedPTY = accepted;
    CHECK(!Local::encodeReclaimWireCapsule(value, wireLimits));
    value.acceptedRuns = {};
    value.acceptedPTY = {};
    auto emptyLimits = wireLimits;
    emptyLimits.maxAcceptedPTYBytes = 0;
    emptyLimits.maxRuns = 0;
    const auto empty = Local::encodeReclaimWireCapsule(value, emptyLimits);
    REQUIRE(empty);
    const auto emptyBytes = record(*empty);
    const auto emptyDecoded = Local::decodeReclaimWireCapsule(emptyBytes, emptyLimits, {});
    REQUIRE(emptyDecoded);
    CHECK(emptyDecoded->acceptedPTY.empty());
    CHECK(emptyDecoded->acceptedRuns.empty());
}

TEST(reclaimWireMissingWrittenReceiptIsDistinctFromKnownZeroAndNeverInvented)
{
    std::array<Local::ReclaimWireRun, 128> sourceRuns {}, destination;
    std::array<PAL::OutputQueue::Run, 128> acceptedRuns {};
    auto missing = wireCapsule(sourceRuns);
    missing.inputWritten.reset();
    const auto encoded = Local::encodeReclaimWireCapsule(missing, wireLimits);
    REQUIRE(encoded);
    const auto absentBytes = record(*encoded);
    const auto absent = Local::decodeReclaimWireCapsule(absentBytes, wireLimits, destination);
    REQUIRE(absent);
    CHECK(!absent->inputWritten);
    CHECK(!reclaimCapsuleFromWire(*absent, selector, correlation, transferLimits, acceptedRuns));
    auto noncanonical = record(*encoded);
    set64(noncanonical, 174, 1);
    CHECK(!Local::decodeReclaimWireCapsule(noncanonical, wireLimits, destination));
    auto zero = missing;
    zero.inputWritten = 0;
    const auto zeroEncoded = Local::encodeReclaimWireCapsule(zero, wireLimits);
    REQUIRE(zeroEncoded);
    const auto zeroBytes = record(*zeroEncoded);
    const auto knownZero = Local::decodeReclaimWireCapsule(zeroBytes, wireLimits, destination);
    REQUIRE(knownZero);
    REQUIRE(knownZero->inputWritten);
    CHECK_EQ(*knownZero->inputWritten, 0u);
    CHECK(!reclaimCapsuleFromWire(*knownZero, selector, correlation, transferLimits, acceptedRuns));

    auto zeroCapsule = capsule();
    zeroCapsule.barrier.finalInputSeq = 0;
    zeroCapsule.inputWritten = 0;
    const auto digest = reclaimCapsuleSHA256(zeroCapsule, transferLimits);
    REQUIRE(digest);
    zeroCapsule.capsuleSHA256 = *digest;
    auto zeroSelector = selector;
    zeroSelector.barrier.finalInputSeq = 0;
    const auto zeroOffer = reclaimCapsuleToWire(adoption, correlation, zeroCapsule, transferLimits, sourceRuns);
    REQUIRE(zeroOffer);
    const auto importedZero = reclaimCapsuleFromWire(*zeroOffer, zeroSelector, correlation, transferLimits, acceptedRuns);
    REQUIRE(importedZero);
    REQUIRE(importedZero->inputWritten);
    CHECK_EQ(*importedZero->inputWritten, 0u);
}

TEST(reclaimWireOutcomesReuseCanonicalAdoptEvidenceAndRequireActualPreparedDigest)
{
    for (const auto disposition : { AdoptDisposition::Unknown, AdoptDisposition::Prepared, AdoptDisposition::Committed,
             AdoptDisposition::Aborted, AdoptDisposition::Expired, AdoptDisposition::Rejected }) {
        auto value = wireOutcome(disposition);
        const auto encoded = Local::encodeReclaimWireOutcome(value);
        REQUIRE(encoded);
        const auto decoded = Local::decodeReclaimWireOutcome(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == value);
        const auto imported = reclaimEvidenceFromWire(*decoded, selector, correlation, capsule().capsuleSHA256);
        REQUIRE(imported);
        CHECK(imported->outcome.disposition == disposition);
        for (std::size_t size = 0; size < encoded->size(); ++size)
            CHECK(!Local::decodeReclaimWireOutcome(std::span(*encoded).first(size)));
        auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
        trailing.push_back(0);
        CHECK(!Local::decodeReclaimWireOutcome(trailing));
        value.capsuleSHA256.reset();
        CHECK_EQ(Local::encodeReclaimWireOutcome(value).has_value(), disposition != AdoptDisposition::Prepared && disposition != AdoptDisposition::Committed);
        CHECK(!reclaimEvidenceFromWire(value, selector, correlation, capsule().capsuleSHA256));
    }
    auto contradictory = wireOutcome();
    ++contradictory.outcome.operation.operationID.low;
    CHECK(!Local::encodeReclaimWireOutcome(contradictory));
    contradictory = wireOutcome();
    ++contradictory.outcome.target.paneID.high;
    CHECK(!Local::encodeReclaimWireOutcome(contradictory));
}

TEST(reclaimWireEverySelectorCorrelationAndDigestMismatchRefusesWithoutScratchMutation)
{
    std::array<Local::ReclaimWireRun, 128> sourceRuns {};
    std::array<PAL::OutputQueue::Run, 128> acceptedRuns;
    std::ranges::fill(acceptedRuns, PAL::OutputQueue::Run { 999, 999 });
    const auto original = wireCapsule(sourceRuns);
    const auto evidence = wireOutcome();
    for (const auto& changed : changedSelectors()) {
        auto offer = original;
        offer.selector = changed;
        CHECK(!reclaimCapsuleFromWire(offer, selector, correlation, transferLimits, acceptedRuns));
        auto answer = evidence;
        answer.selector = changed;
        CHECK(!reclaimEvidenceFromWire(answer, selector, correlation, capsule().capsuleSHA256));
    }
    auto wrongCorrelation = original;
    ++wrongCorrelation.prepared.correlation;
    CHECK(!reclaimCapsuleFromWire(wrongCorrelation, selector, correlation, transferLimits, acceptedRuns));
    for (std::size_t index = 0; index < 32; ++index) {
        auto badDigest = original;
        badDigest.capsuleSHA256[index] ^= 1;
        CHECK(!reclaimCapsuleFromWire(badDigest, selector, correlation, transferLimits, acceptedRuns));
        auto answer = evidence;
        (*answer.capsuleSHA256)[index] ^= 1;
        CHECK(!reclaimEvidenceFromWire(answer, selector, correlation, capsule().capsuleSHA256));
    }
    CHECK(std::ranges::all_of(acceptedRuns, [](const auto& run) { return run.owner == 999 && run.bytes == 999; }));
}

TEST(reclaimWireRefusalCannotBeReadAsUnknownOrReleasedEvidence)
{
    for (const auto reason : { AdoptTransactionError::InvalidIdentity, AdoptTransactionError::InvalidPolicy, AdoptTransactionError::NoMemory,
             AdoptTransactionError::Busy, AdoptTransactionError::StaleAuthority, AdoptTransactionError::Unauthorised,
             AdoptTransactionError::Conflict, AdoptTransactionError::Reentrant, AdoptTransactionError::ReplyUnavailable,
             AdoptTransactionError::InvalidTime, AdoptTransactionError::GenerationExhausted, AdoptTransactionError::ActiveCandidate,
             AdoptTransactionError::RetirementUnproven }) {
        const auto value = reclaimRefusedToWire(selector, reason, correlation, capsule().capsuleSHA256);
        REQUIRE(value);
        const auto encoded = Local::encodeReclaimWireRefused(*value);
        REQUIRE(encoded);
        CHECK(!Local::decodeReclaimWireOutcome(*encoded));
        const auto decoded = Local::decodeReclaimWireRefused(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == *value);
        const auto imported = reclaimRefusedFromWire(*decoded, selector, correlation, capsule().capsuleSHA256);
        REQUIRE(imported);
        CHECK(*imported == reason);
        CHECK(!reclaimRefusedFromWire(*decoded, selector, correlation + 1, capsule().capsuleSHA256));
        CHECK(!reclaimRefusedFromWire(*decoded, selector, correlation, std::nullopt));
        for (std::size_t size = 0; size < encoded->size(); ++size)
            CHECK(!Local::decodeReclaimWireRefused(std::span(*encoded).first(size)));
        auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
        trailing.push_back(0);
        CHECK(!Local::decodeReclaimWireRefused(trailing));
    }
}

TEST(reclaimWireRealCompleteTerminalAndSuffixReachTheExistingIrreversibleGate)
{
    Terminal terminal;
    terminal.scrollbackLimit = 32;
    resize(terminal, 5, 24);
    UbiquittyTest::feed(terminal, "history\r\nhistory\r\nhistory\r\nhistory\r\nhistory\r\nhistory\r\n"
                                  "\033]133;A\007prompt\033]133;B\007\0337\033[2;4r\033[6n");
    terminal.answerback = "complete reclaim owner";
    const auto body = Sync::encodeHandoverTerminal(terminal, 9, terminalLimits);
    REQUIRE(body);
    const auto original = capsule(body->view());
    std::array<Local::ReclaimWireRun, 128> sourceRuns {}, decodedRuns {};
    std::array<PAL::OutputQueue::Run, 128> importedRuns {};
    const auto offered = reclaimCapsuleToWire(adoption, correlation, original, transferLimits, sourceRuns);
    REQUIRE(offered);
    const auto encoded = Local::encodeReclaimWireCapsule(*offered, wireLimits);
    REQUIRE(encoded);
    const auto bytes = record(*encoded);
    const auto decoded = Local::decodeReclaimWireCapsule(bytes, wireLimits, decodedRuns);
    REQUIRE(decoded);
    const auto imported = reclaimCapsuleFromWire(*decoded, selector, correlation, transferLimits, importedRuns);
    REQUIRE(imported);
    auto prepared = ReclaimTransfer::prepare(target, operation, barrier, *imported, transferLimits);
    REQUIRE(prepared);
    auto& transfer = **prepared;
    REQUIRE(transfer.beginCommit());
    for (const auto disposition : { AdoptDisposition::Unknown, AdoptDisposition::Prepared }) {
        auto answer = wireOutcome(disposition);
        answer.capsuleSHA256 = original.capsuleSHA256;
        const auto evidence = reclaimEvidenceFromWire(answer, selector, correlation, original.capsuleSHA256);
        REQUIRE(evidence);
        REQUIRE(transfer.observe(*evidence));
        CHECK(transfer.ownership() == ReclaimOwnership::Unresolved);
        CHECK(transfer.hasPreparedInstallation());
        CHECK(!transfer.takeCommitted());
    }
    auto answer = wireOutcome();
    answer.capsuleSHA256 = original.capsuleSHA256;
    const auto committed = reclaimEvidenceFromWire(answer, selector, correlation, original.capsuleSHA256);
    REQUIRE(committed);
    REQUIRE(transfer.observe(*committed));
    auto installation = transfer.takeCommitted();
    REQUIRE(installation && installation->terminal.terminal);
    CHECK(transfer.ownership() == ReclaimOwnership::Local);
    CHECK(!transfer.takeCommitted());
    const auto roundtrip = Sync::encodeHandoverTerminal(*installation->terminal.terminal, 9, terminalLimits);
    REQUIRE(roundtrip);
    CHECK(std::ranges::equal(roundtrip->view(), original.terminalBody));
    CHECK(std::string_view(installation->acceptedPTY.bytes().data(), installation->acceptedPTY.bytes().size()) == "defreply");
    REQUIRE(installation->acceptedPTY.runs().size() == 2);
    CHECK_EQ(installation->acceptedPTY.runs()[0].owner, 81u);
    CHECK_EQ(installation->acceptedPTY.runs()[1].owner, 0u);
}

UBIQUITTY_TEST_MAIN()
