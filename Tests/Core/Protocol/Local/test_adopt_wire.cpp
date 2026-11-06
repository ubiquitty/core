// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/AdoptWire.h"

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"

#include "Core/Crypto/SHA256.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace Local = Ubiquitty::Protocol::Local;

Local::AdoptWireOperation operation()
{
    return { .operationID = { .low = 0x0807060504030201, .high = 0x1817161514131211 },
        .daemonInstanceID = 0x2827262524232221 };
}

constexpr std::uint64_t correlation = 0x3837363534333231;
constexpr std::array<std::uint8_t, 7> terminalBody { 0, 0xFF, 0x1B, 7, 0, 0x80, 1 };

Local::AdoptWirePrepare prepare(std::span<const std::uint8_t> body = terminalBody)
{
    Local::AdoptWirePrepare result { .correlation = correlation,
        .preparation = { .operation = operation(), .bodyBytes = body.size() },
        .terminalBody = body };
    Ubiquitty::Crypto::sha256(body, result.preparation.bodySHA256);
    return result;
}

std::vector<std::uint8_t> prepareRecord(const Local::EncodedAdoptWirePrepare& encoded)
{
    std::vector<std::uint8_t> bytes(encoded.header.begin(), encoded.header.end());
    bytes.insert(bytes.end(), encoded.terminalBody.begin(), encoded.terminalBody.end());
    return bytes;
}

Local::AdoptWireTarget target()
{
    return { .sessionID = 0x48474645, .paneID = { .low = 0x5857565554535251, .high = 0x6867666564636261 } };
}

Local::AdoptWireOutcome outcome()
{
    return { .correlation = correlation, .operation = operation(), .disposition = Local::AdoptWireDisposition::Committed, .target = target() };
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

void clearIdentity(std::span<std::uint8_t> bytes, std::size_t field)
{
    constexpr std::array<std::size_t, 3> offsets { 5, 13, 29 };
    const std::size_t length = field == 1 ? 16 : 8;
    std::ranges::fill(bytes.subspan(offsets[field], length), 0);
}

} // namespace

TEST(adoptWirePrepareUsesTheExactFiveByteLocalFrameAndKnownDigest)
{
    const std::array<std::uint8_t, 3> body { 'a', 'b', 'c' };
    const std::array<std::uint8_t, 32> digest {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C, 0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
    };
    auto request = prepare(body);
    CHECK(request.preparation.bodySHA256 == digest);
    const auto encoded = Local::encodeAdoptWirePrepare(request, 80);
    REQUIRE(encoded.has_value());
    CHECK_EQ(encoded->header.size(), 77u);
    const std::array<std::uint8_t, 5> frame { 1, 75, 0, 0, 0 };
    CHECK(std::ranges::equal(std::span(encoded->header).first<5>(), frame));
    const std::array<std::uint8_t, 32> requestBytes {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28
    };
    CHECK(std::ranges::equal(std::span(encoded->header).subspan<5, 32>(), requestBytes));
    CHECK_EQ(encoded->header[37], 3u);
    CHECK(std::ranges::all_of(std::span(encoded->header).subspan<38, 7>(), [](auto byte) { return !byte; }));
    CHECK(std::ranges::equal(std::span(encoded->header).last<32>(), digest));
    CHECK(encoded->terminalBody.data() == body.data());
}

TEST(adoptWirePrepareBorrowsEveryOpaqueTerminalByteWithoutCheckpointParsing)
{
    const auto original = prepare();
    const std::size_t length = Local::adoptWirePrepareHeaderBytes + terminalBody.size();
    const auto encoded = Local::encodeAdoptWirePrepare(original, length);
    REQUIRE(encoded.has_value());
    CHECK(encoded->terminalBody.data() == terminalBody.data());
    const auto bytes = prepareRecord(*encoded);
    const auto decoded = Local::decodeAdoptWirePrepare(bytes, length);
    REQUIRE(decoded.has_value());
    CHECK_EQ(decoded->correlation, correlation);
    CHECK(decoded->preparation == original.preparation);
    CHECK(decoded->terminalBody.data() == bytes.data() + Local::adoptWirePrepareHeaderBytes);
    CHECK(std::ranges::equal(decoded->terminalBody, terminalBody));
    const auto repeated = Local::encodeAdoptWirePrepare(*decoded, length);
    REQUIRE(repeated.has_value());
    CHECK(prepareRecord(*repeated) == bytes);
}

TEST(adoptWirePrepareChecksItsCompleteAllowanceBeforeHashing)
{
    auto request = prepare();
    const std::size_t length = Local::adoptWirePrepareHeaderBytes + terminalBody.size();
    CHECK(Local::encodeAdoptWirePrepare(request, length).has_value());
    request.preparation.bodySHA256[0] ^= 1;
    for (const auto allowance : std::array<std::size_t, 3> { 0, Local::adoptWirePrepareHeaderBytes, length - 1 }) {
        const auto refused = Local::encodeAdoptWirePrepare(request, allowance);
        REQUIRE(!refused.has_value());
        CHECK(refused.error() == Local::AdoptWireError::TooLong);
    }
    const auto refused = Local::encodeAdoptWirePrepare(request, length);
    REQUIRE(!refused.has_value());
    CHECK(refused.error() == Local::AdoptWireError::DigestMismatch);
    auto valid = Local::encodeAdoptWirePrepare(prepare(), length);
    REQUIRE(valid.has_value());
    auto bytes = prepareRecord(*valid);
    bytes.back() ^= 1;
    const auto decoded = Local::decodeAdoptWirePrepare(bytes, length - 1);
    REQUIRE(!decoded.has_value());
    CHECK(decoded.error() == Local::AdoptWireError::TooLong);
}

TEST(adoptWirePrepareRejectsEmptyAndContradictoryBodyLengths)
{
    CHECK(!Local::encodeAdoptWirePrepare(prepare({}), 100));
    auto request = prepare();
    for (const auto length : std::array<std::uint64_t, 4> { 0, terminalBody.size() - 1, terminalBody.size() + 1,
             std::numeric_limits<std::uint64_t>::max() }) {
        request.preparation.bodyBytes = length;
        const auto refused = Local::encodeAdoptWirePrepare(request, 100);
        REQUIRE(!refused.has_value());
        CHECK(refused.error() == Local::AdoptWireError::InvalidPayload);
    }
    const auto encoded = Local::encodeAdoptWirePrepare(prepare(), 100);
    REQUIRE(encoded.has_value());
    const auto original = prepareRecord(*encoded);
    for (const auto length : std::array<std::uint64_t, 4> { 0, terminalBody.size() - 1, terminalBody.size() + 1,
             std::numeric_limits<std::uint64_t>::max() }) {
        auto bytes = original;
        set64(bytes, Local::adoptWireCommandBytes, length);
        CHECK(!Local::decodeAdoptWirePrepare(bytes, 100));
    }
}

TEST(adoptWirePrepareRejectsEveryTruncationAndAllTrailingBytes)
{
    const auto encoded = Local::encodeAdoptWirePrepare(prepare(), 100);
    REQUIRE(encoded.has_value());
    auto bytes = prepareRecord(*encoded);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK(!Local::decodeAdoptWirePrepare(std::span(bytes).first(length), 100));
    bytes.push_back(0);
    CHECK(!Local::decodeAdoptWirePrepare(bytes, 100));
    ++bytes[1];
    CHECK(!Local::decodeAdoptWirePrepare(bytes, 100));
}

TEST(adoptWirePrepareChecksEveryDigestByteAndTheEntireBody)
{
    const auto encoded = Local::encodeAdoptWirePrepare(prepare(), 100);
    REQUIRE(encoded.has_value());
    const auto original = prepareRecord(*encoded);
    for (std::size_t index = 45; index < original.size(); ++index) {
        auto bytes = original;
        bytes[index] ^= 1;
        const auto refused = Local::decodeAdoptWirePrepare(bytes, 100);
        REQUIRE(!refused.has_value());
        CHECK(refused.error() == Local::AdoptWireError::DigestMismatch);
    }
}

TEST(adoptWireHeaderValidatesEveryKindAndExactFixedLength)
{
    for (unsigned value = 0; value <= 255; ++value) {
        const auto kind = static_cast<Local::AdoptWireMessage>(value);
        const bool isKnown = kind == Local::AdoptWireMessage::Prepare || kind == Local::AdoptWireMessage::Commit
            || kind == Local::AdoptWireMessage::Abort || kind == Local::AdoptWireMessage::Status
            || kind == Local::AdoptWireMessage::Outcome || kind == Local::AdoptWireMessage::Refused;
        const std::uint8_t bodyBytes = kind == Local::AdoptWireMessage::Prepare ? 73
            : kind == Local::AdoptWireMessage::Outcome                          ? 54
            : kind == Local::AdoptWireMessage::Refused                          ? 33
                                                                                : 32;
        std::array<std::uint8_t, 5> header { static_cast<std::uint8_t>(value), bodyBytes, 0, 0, 0 };
        CHECK_EQ(Local::decodeAdoptWireHeader(header, 100).has_value(), isKnown);
        for (std::size_t length = 0; length < header.size(); ++length)
            CHECK(!Local::decodeAdoptWireHeader(std::span(header).first(length), 100));
        if (isKnown) {
            --header[1];
            CHECK(!Local::decodeAdoptWireHeader(header, 100));
            header[1] += 2;
            CHECK_EQ(Local::decodeAdoptWireHeader(header, 100).has_value(), kind == Local::AdoptWireMessage::Prepare);
        }
    }
    const std::array<std::uint8_t, 6> trailing { 2, 32, 0, 0, 0, 0 };
    CHECK(!Local::decodeAdoptWireHeader(trailing, 100));
}

TEST(adoptWireHeaderBoundsU32LengthAndWholeRecordAdditionWithoutAllocating)
{
    const std::array<std::uint8_t, 5> header { 1, 0xFF, 0xFF, 0xFF, 0xFF };
    const auto limited = Local::decodeAdoptWireHeader(header, 100);
    REQUIRE(!limited.has_value());
    CHECK(limited.error() == Local::AdoptWireError::TooLong);
    const auto representable = Local::decodeAdoptWireHeader(header, std::numeric_limits<std::size_t>::max());
    const std::uint64_t recordBytes = std::uint64_t { std::numeric_limits<std::uint32_t>::max() } + 5;
    CHECK_EQ(representable.has_value(), recordBytes <= std::numeric_limits<std::size_t>::max());
    CHECK(!Local::decodeAdoptWirePrepare(header, 100));
}

TEST(adoptWireCommandsPreserveCorrelationAndBothOperationHalves)
{
    for (const auto kind : { Local::AdoptWireMessage::Commit, Local::AdoptWireMessage::Abort, Local::AdoptWireMessage::Status }) {
        const Local::AdoptWireCommand command { .kind = kind, .correlation = correlation, .operation = operation() };
        const auto encoded = Local::encodeAdoptWireCommand(command);
        REQUIRE(encoded.has_value());
        CHECK_EQ(encoded->size(), 37u);
        CHECK_EQ(encoded->front(), std::to_underlying(kind));
        CHECK_EQ((*encoded)[1], 32u);
        const auto decoded = Local::decodeAdoptWireCommand(*encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == command);
        for (std::size_t length = 0; length < encoded->size(); ++length)
            CHECK(!Local::decodeAdoptWireCommand(std::span(*encoded).first(length)));
        auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
        trailing.push_back(0);
        CHECK(!Local::decodeAdoptWireCommand(trailing));
    }
}

TEST(adoptWireCommandsRejectAllOtherKindsAndBadFrameLengths)
{
    for (unsigned value = 0; value <= 255; ++value) {
        const auto kind = static_cast<Local::AdoptWireMessage>(value);
        const bool isCommand = value >= 2 && value <= 4;
        CHECK_EQ(Local::encodeAdoptWireCommand({ .kind = kind, .correlation = correlation, .operation = operation() }).has_value(), isCommand);
    }
    const auto encoded = Local::encodeAdoptWireCommand({ .correlation = correlation, .operation = operation() });
    REQUIRE(encoded.has_value());
    auto bytes = *encoded;
    for (unsigned value = 0; value <= 255; ++value) {
        bytes[0] = static_cast<std::uint8_t>(value);
        CHECK_EQ(Local::decodeAdoptWireCommand(bytes).has_value(), value >= 2 && value <= 4);
    }
    bytes = *encoded;
    --bytes[1];
    CHECK(!Local::decodeAdoptWireCommand(bytes));
    bytes[1] += 2;
    CHECK(!Local::decodeAdoptWireCommand(bytes));
}

TEST(adoptWireAllRecordFamiliesRejectZeroCorrelationOperationAndDaemonPins)
{
    for (std::size_t field = 0; field < 3; ++field) {
        auto request = prepare();
        auto command = Local::AdoptWireCommand { .correlation = correlation, .operation = operation() };
        auto answer = outcome();
        auto refused = Local::AdoptWireRefused { .correlation = correlation, .operation = operation() };
        if (!field) {
            request.correlation = command.correlation = answer.correlation = refused.correlation = 0;
        } else if (field == 1) {
            request.preparation.operation.operationID = command.operation.operationID = answer.operation.operationID = refused.operation.operationID = {};
        } else {
            request.preparation.operation.daemonInstanceID = command.operation.daemonInstanceID = answer.operation.daemonInstanceID = refused.operation.daemonInstanceID = 0;
        }
        CHECK(!Local::encodeAdoptWirePrepare(request, 100));
        CHECK(!Local::encodeAdoptWireCommand(command));
        CHECK(!Local::encodeAdoptWireOutcome(answer));
        CHECK(!Local::encodeAdoptWireRefused(refused));
        const auto validPrepare = Local::encodeAdoptWirePrepare(prepare(), 100);
        const auto validCommand = Local::encodeAdoptWireCommand({ .correlation = correlation, .operation = operation() });
        const auto validOutcome = Local::encodeAdoptWireOutcome(outcome());
        const auto validRefused = Local::encodeAdoptWireRefused({ .correlation = correlation, .operation = operation() });
        REQUIRE(validPrepare && validCommand && validOutcome && validRefused);
        auto prepareBytes = prepareRecord(*validPrepare);
        auto commandBytes = *validCommand;
        auto outcomeBytes = *validOutcome;
        auto refusedBytes = *validRefused;
        clearIdentity(prepareBytes, field);
        clearIdentity(commandBytes, field);
        clearIdentity(outcomeBytes, field);
        clearIdentity(refusedBytes, field);
        CHECK(!Local::decodeAdoptWirePrepare(prepareBytes, 100));
        CHECK(!Local::decodeAdoptWireCommand(commandBytes));
        CHECK(!Local::decodeAdoptWireOutcome(outcomeBytes));
        CHECK(!Local::decodeAdoptWireRefused(refusedBytes));
    }
}

TEST(adoptWireOperationMayUseEitherHalfWithoutNarrowingItsIdentity)
{
    for (const auto identity : { Ubiquitty::Protocol::Sync::ID128 { 0, 9 }, Ubiquitty::Protocol::Sync::ID128 { 9, 0 } }) {
        auto command = Local::AdoptWireCommand { .correlation = correlation, .operation = operation() };
        command.operation.operationID = identity;
        const auto encoded = Local::encodeAdoptWireCommand(command);
        REQUIRE(encoded.has_value());
        const auto decoded = Local::decodeAdoptWireCommand(*encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->operation.operationID == identity);
    }
}

TEST(adoptWireOutcomeShapesMatchEveryDispositionTargetAndRejectionCombination)
{
    const auto valid = Local::encodeAdoptWireOutcome(outcome());
    REQUIRE(valid.has_value());
    const std::array<Local::AdoptWireTarget, 4> targets { Local::AdoptWireTarget {}, target(),
        Local::AdoptWireTarget { .sessionID = 7 }, Local::AdoptWireTarget { .paneID = { 8, 9 } } };
    const std::array<std::optional<Local::AdoptWireRejection>, 8> rejections { std::nullopt,
        Local::AdoptWireRejection::InvalidPayload, Local::AdoptWireRejection::NotOwner,
        Local::AdoptWireRejection::NoMemory, Local::AdoptWireRejection::InvalidCandidate,
        Local::AdoptWireRejection::HostFull,
        static_cast<Local::AdoptWireRejection>(0), static_cast<Local::AdoptWireRejection>(255) };
    // The defined reasons, between the absent one and the two invalid bytes.
    constexpr std::size_t lastDefinedRejection = 5;
    constexpr std::size_t zeroRejection = 6;
    for (unsigned disposition = 0; disposition <= 5; ++disposition) {
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            for (std::size_t rejectionIndex = 0; rejectionIndex < rejections.size(); ++rejectionIndex) {
                auto answer = outcome();
                answer.disposition = static_cast<Local::AdoptWireDisposition>(disposition);
                answer.target = targets[targetIndex];
                answer.rejection = rejections[rejectionIndex];
                bool isAllowed = targetIndex < 2;
                if (disposition == 0)
                    isAllowed = isAllowed && targetIndex == 0 && !rejectionIndex;
                else if (disposition == 1 || disposition == 2)
                    isAllowed = isAllowed && targetIndex == 1 && !rejectionIndex;
                else if (disposition == 3 || disposition == 4)
                    isAllowed = isAllowed && !rejectionIndex;
                else
                    isAllowed = isAllowed && rejectionIndex >= 1 && rejectionIndex <= lastDefinedRejection;
                const auto encoded = Local::encodeAdoptWireOutcome(answer);
                CHECK_EQ(encoded.has_value(), isAllowed);
                if (encoded) {
                    const auto decoded = Local::decodeAdoptWireOutcome(*encoded);
                    REQUIRE(decoded.has_value());
                    CHECK(*decoded == answer);
                }
                if (rejectionIndex != zeroRejection) {
                    auto bytes = *valid;
                    bytes[37] = static_cast<std::uint8_t>(disposition);
                    set32(bytes, 38, answer.target.sessionID);
                    set64(bytes, 42, answer.target.paneID.low);
                    set64(bytes, 50, answer.target.paneID.high);
                    bytes.back() = answer.rejection ? std::to_underlying(*answer.rejection) : 0;
                    CHECK_EQ(Local::decodeAdoptWireOutcome(bytes).has_value(), isAllowed);
                }
            }
        }
    }
}

TEST(adoptWireOutcomeRejectsEveryUnknownDispositionAndRejectionByte)
{
    const auto encoded = Local::encodeAdoptWireOutcome(outcome());
    REQUIRE(encoded.has_value());
    for (unsigned value = 0; value <= 255; ++value) {
        auto bytes = *encoded;
        bytes[37] = static_cast<std::uint8_t>(value);
        const bool isAllowed = value >= 1 && value <= 4;
        CHECK_EQ(Local::decodeAdoptWireOutcome(bytes).has_value(), isAllowed);
        auto answer = outcome();
        answer.disposition = static_cast<Local::AdoptWireDisposition>(value);
        CHECK_EQ(Local::encodeAdoptWireOutcome(answer).has_value(), isAllowed);
        bytes = *encoded;
        bytes[37] = 5;
        bytes.back() = static_cast<std::uint8_t>(value);
        CHECK_EQ(Local::decodeAdoptWireOutcome(bytes).has_value(), value >= 1 && value <= 5);
    }
}

TEST(adoptWireOutcomeRejectsPartialTargetsAndAllTruncationOrTrailingData)
{
    const auto encoded = Local::encodeAdoptWireOutcome(outcome());
    REQUIRE(encoded.has_value());
    for (std::size_t length = 0; length < encoded->size(); ++length)
        CHECK(!Local::decodeAdoptWireOutcome(std::span(*encoded).first(length)));
    auto bytes = *encoded;
    std::ranges::fill(std::span(bytes).subspan<38, 4>(), 0);
    CHECK(!Local::decodeAdoptWireOutcome(bytes));
    bytes = *encoded;
    std::ranges::fill(std::span(bytes).subspan<42, 16>(), 0);
    CHECK(!Local::decodeAdoptWireOutcome(bytes));
    auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
    trailing.push_back(0);
    CHECK(!Local::decodeAdoptWireOutcome(trailing));
    ++trailing[1];
    CHECK(!Local::decodeAdoptWireOutcome(trailing));
}

TEST(adoptWireRefusalChecksEveryExplicitReasonWithoutCreatingAnOutcome)
{
    for (unsigned value = 0; value <= 255; ++value) {
        Local::AdoptWireRefused refused { .correlation = correlation, .operation = operation(), .reason = static_cast<Local::AdoptWireRefusal>(value) };
        const auto encoded = Local::encodeAdoptWireRefused(refused);
        const bool isKnown = value >= 1 && value <= 13;
        CHECK_EQ(encoded.has_value(), isKnown);
        if (encoded) {
            CHECK_EQ(encoded->size(), 38u);
            const auto decoded = Local::decodeAdoptWireRefused(*encoded);
            REQUIRE(decoded.has_value());
            CHECK(*decoded == refused);
            CHECK(!Local::decodeAdoptWireOutcome(*encoded));
            CHECK(!Local::decodeAdoptWireCommand(*encoded));
            CHECK(!Local::decodeAdoptWirePrepare(*encoded, 100));
        }
    }
}

TEST(adoptWireRefusalRejectsMalformedReasonLengthAndIdentityWithoutPartialResults)
{
    const auto encoded = Local::encodeAdoptWireRefused({ .correlation = correlation, .operation = operation() });
    REQUIRE(encoded.has_value());
    for (unsigned value = 0; value <= 255; ++value) {
        auto bytes = *encoded;
        bytes.back() = static_cast<std::uint8_t>(value);
        CHECK_EQ(Local::decodeAdoptWireRefused(bytes).has_value(), value >= 1 && value <= 13);
    }
    for (std::size_t length = 0; length < encoded->size(); ++length)
        CHECK(!Local::decodeAdoptWireRefused(std::span(*encoded).first(length)));
    auto trailing = std::vector<std::uint8_t>(encoded->begin(), encoded->end());
    trailing.push_back(0);
    CHECK(!Local::decodeAdoptWireRefused(trailing));
    ++trailing[1];
    CHECK(!Local::decodeAdoptWireRefused(trailing));
    auto wrongKind = *encoded;
    wrongKind[0] = std::to_underlying(Local::AdoptWireMessage::Outcome);
    CHECK(!Local::decodeAdoptWireRefused(wrongKind));
}

TEST(adoptWireEncodingRefusalLeavesBorrowedInputUnchanged)
{
    auto bytes = std::vector<std::uint8_t>(terminalBody.begin(), terminalBody.end());
    const auto original = bytes;
    auto request = prepare(bytes);
    request.preparation.bodySHA256[0] ^= 1;
    const auto refused = Local::encodeAdoptWirePrepare(request, 100);
    CHECK(!refused.has_value());
    CHECK(bytes == original);
    CHECK_EQ(request.correlation, correlation);
    CHECK(request.preparation.operation == operation());
    auto answer = outcome();
    answer.disposition = Local::AdoptWireDisposition::Unknown;
    const auto unchanged = answer;
    CHECK(!Local::encodeAdoptWireOutcome(answer));
    CHECK(answer == unchanged);
}

UBIQUITTY_TEST_MAIN()
