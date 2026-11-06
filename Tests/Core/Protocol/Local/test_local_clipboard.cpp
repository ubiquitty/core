// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/Clipboard.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "StreamPartitions.h"

#include <algorithm>
#include <array>
#include <functional>
#include <new>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol;
using namespace Ubiquitty::Protocol::Local;
using UbiquittyTest::ScopedNothrowAllocationRefusal;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

using State = LocalClipboardRequestReader::State;
constexpr std::uint64_t correlation = 0x0807060504030201;
constexpr std::uint64_t daemon = 0x1817161514131211;
constexpr std::size_t requestHeaderBytes = 7;
constexpr std::size_t requestKindOffset = 4;
constexpr std::size_t requestMIMEOffset = 5;
constexpr std::size_t replyStatusOffset = localControlPreamble.size() + 1;
constexpr std::size_t replyCorrelationOffset = localControlPreamble.size() + 2;
constexpr std::size_t replyDaemonOffset = localControlReplyBytes - 8;
constexpr std::size_t replySelectionOffset = localControlReplyBytes;
constexpr std::size_t replyLengthOffset = localControlReplyBytes + 1;

/// Pokes only fixture length/identity fields; no test-side decoder is used.
void putLittleEndian(std::span<std::uint8_t> bytes, std::uint64_t value)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

/// Owns bytes produced by the actual request encoder for later fragmentation.
std::vector<std::uint8_t> requestBytes(LocalClipboardKind kind, std::string_view mime = {})
{
    const auto encoded = encodeLocalClipboardRequest(correlation, { kind, mime });
    CHECK(encoded.has_value());
    return encoded ? std::vector<std::uint8_t>(encoded->view().begin(), encoded->view().end()) : std::vector<std::uint8_t> {};
}

/// Owns a complete reply written by the production writer into admitted storage.
std::vector<std::uint8_t> replyBytes(const LocalClipboardReply& reply)
{
    const auto size = localClipboardReplyBytes(reply);
    CHECK(size.has_value());
    if (!size)
        return {};
    std::vector<std::uint8_t> bytes(*size, 0xa5);
    if (!writeLocalClipboardReply(reply, bytes)) {
        CHECK(false);
        return {};
    }
    return bytes;
}

/// A small binary fixture whose storage outlives every borrowed bundle view.
struct BinaryClipboard {
    std::array<std::uint8_t, 6> data { 0, 255, 1, 128, 13, 10 };
    Sync::EffectClipboardBundle bundle() const
    {
        Sync::EffectClipboardBundle result {};
        result.count = 1;
        result.items[0] = { "application/octet-stream", data };
        return result;
    }
};

/// Composes only the production first-packet router and operation-body reader.
/// The prefix's consumed byte count, not a second grammar, identifies the suffix.
struct RequestExchange {
    LocalControlReader prefix;
    LocalClipboardRequestReader body;
    std::optional<LocalControlRequest> operation;
    State state = State::Awaiting;

    State consume(std::span<const std::uint8_t> bytes)
    {
        if (!operation) {
            const auto prefixState = prefix.consume(bytes);
            if (prefixState == LocalControlReader::State::Refused || prefixState == LocalControlReader::State::Ordinary)
                return state = State::Refused;
            if (prefixState != LocalControlReader::State::Ready)
                return state;
            const auto consumed = prefix.consumedBytes();
            operation = prefix.take();
            if (!operation || operation->operation != LocalControlOperation::Clipboard)
                return state = State::Refused;
            bytes = bytes.subspan(consumed);
        }
        return state = body.consume(bytes);
    }
};

/// Confirms complete reply views, including all binary bytes, while owners live.
void checkReply(const LocalClipboardReply& actual, const LocalClipboardReply& expected)
{
    CHECK_EQ(actual.correlation, expected.correlation);
    CHECK_EQ(actual.daemonInstanceID, expected.daemonInstanceID);
    CHECK(actual.status == expected.status);
    CHECK(actual.isHostSelected == expected.isHostSelected);
    REQUIRE(actual.bundle.count == expected.bundle.count);
    for (std::size_t index = 0; index < actual.bundle.count; ++index) {
        CHECK(actual.bundle.items[index].mime == expected.bundle.items[index].mime);
        CHECK(std::ranges::equal(actual.bundle.items[index].data, expected.bundle.items[index].data));
    }
}

TEST(localClipboardPrefixKeepsOneRouteAndFrozenReloadQUICBytes)
{
    const auto clipboard = requestBytes(LocalClipboardKind::Types);
    REQUIRE(clipboard.size() == localControlRequestBytes + requestHeaderBytes);
    LocalControlReader prefix;
    CHECK(prefix.consume(clipboard) == LocalControlReader::State::Ready);
    CHECK_EQ(prefix.consumedBytes(), localControlRequestBytes);
    const auto selected = prefix.take();
    REQUIRE(selected);
    CHECK(selected->operation == LocalControlOperation::Clipboard);
    CHECK_EQ(selected->correlation, correlation);
    CHECK(prefix.consume(clipboard) == LocalControlReader::State::Consumed);
    CHECK_EQ(prefix.consumedBytes(), 0u);
    CHECK(!prefix.take());

    for (const auto first : { std::uint8_t { 0x01 }, std::uint8_t { 0xf8 }, std::uint8_t { 0xfa } }) {
        LocalControlReader ordinary;
        CHECK(ordinary.consume({ &first, 1 }) == LocalControlReader::State::Ordinary);
        CHECK_EQ(ordinary.consumedBytes(), 0u);
        CHECK(ordinary.consume(clipboard) == LocalControlReader::State::Ordinary);
        CHECK(!ordinary.take());
    }
    const auto quic = encodeLocalQUICTokenRequest(correlation);
    REQUIRE(quic);
    for (const auto operation : { localReloadOperation, localQUICTokenOperation }) {
        const auto fixed = operation == localReloadOperation ? encodeLocalReloadRequest(correlation) : *quic;
        CHECK(std::ranges::equal(std::span { fixed }.first(localControlPreamble.size()), localControlPreamble));
        CHECK_EQ(fixed[16], operation);
        for (std::size_t index = 0; index < 8; ++index)
            CHECK_EQ(fixed[17 + index], index + 1);
        auto trailing = std::vector<std::uint8_t>(fixed.begin(), fixed.end());
        trailing.push_back(0);
        for (std::size_t split = 0; split < fixed.size(); ++split) {
            LocalControlReader reader;
            CHECK(reader.consume(std::span { trailing }.first(split)) == LocalControlReader::State::Awaiting);
            CHECK(reader.consume(std::span { trailing }.subspan(split)) == LocalControlReader::State::Refused);
            CHECK(!reader.take());
        }
        LocalControlReader consumed;
        CHECK(consumed.consume(fixed) == LocalControlReader::State::Ready);
        CHECK(consumed.take().has_value());
        // This later-chunk behavior is the frozen op1/op2 contract, not op4's.
        CHECK(consumed.consume(trailing) == LocalControlReader::State::Consumed);
    }
    auto zero = clipboard;
    putLittleEndian(std::span { zero }.subspan(17, 8), 0);
    LocalControlReader refused;
    CHECK(refused.consume(zero) == LocalControlReader::State::Refused);
    CHECK(refused.consume(clipboard) == LocalControlReader::State::Refused);
    CHECK(!refused.take());
}

TEST(localClipboardRequestsSurviveEveryPrefixAndBodySplit)
{
    const std::string unicodeMIME = "text/plain;name=\xc3\xa9\xf0\x9f\x90\x88";
    for (const auto request : { LocalClipboardRequest { LocalClipboardKind::Types, {} },
             LocalClipboardRequest { LocalClipboardKind::Item, unicodeMIME },
             LocalClipboardRequest { LocalClipboardKind::Route, {} } }) {
        const auto bytes = requestBytes(request.kind, request.mime);
        REQUIRE(!bytes.empty());
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            RequestExchange exchange;
            CHECK(exchange.consume(std::span { bytes }.first(split)) == (split == bytes.size() ? State::Ready : State::Awaiting));
            if (split < bytes.size())
                CHECK(!exchange.body.take());
            CHECK(exchange.consume(std::span { bytes }.subspan(split)) == State::Ready);
            REQUIRE(exchange.operation);
            CHECK_EQ(exchange.operation->correlation, correlation);
            const auto decoded = exchange.body.take();
            REQUIRE(decoded);
            CHECK(decoded->kind == request.kind);
            CHECK(decoded->mime == request.mime);
            CHECK(!exchange.body.take());
            CHECK(exchange.consume({}) == State::Consumed);
            CHECK(exchange.consume(bytes) == State::Refused);
        }
        RequestExchange bytewise;
        for (std::size_t index = 0; index < bytes.size(); ++index)
            CHECK(bytewise.consume(std::span { bytes }.subspan(index, 1)) == (index + 1 == bytes.size() ? State::Ready : State::Awaiting));
        CHECK(bytewise.body.take().has_value());
        auto trailing = bytes;
        trailing.push_back(0);
        RequestExchange coalesced;
        CHECK(coalesced.consume(trailing) == State::Refused);
        CHECK(!coalesced.body.take());
    }
}

TEST(localClipboardRequestHeadersRefuseBeforeMIMEAllocation)
{
    const auto encoded = requestBytes(LocalClipboardKind::Item, "text/plain");
    REQUIRE(encoded.size() > localControlRequestBytes + requestHeaderBytes);
    const auto body = std::span { encoded }.subspan(localControlRequestBytes);
    for (std::size_t split = 0; split < requestHeaderBytes; ++split) {
        LocalClipboardRequestReader reader;
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        ScopedNothrowAllocationRefusal refuse;
        CHECK(reader.consume(body.first(split)) == State::Awaiting);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK(reader.consume(body.subspan(split, requestHeaderBytes - split)) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 1);
        CHECK(!reader.take());
    }
    for (std::size_t mutation = 0; mutation < 9; ++mutation) {
        auto invalid = std::vector<std::uint8_t>(body.begin(), body.begin() + requestHeaderBytes);
        switch (mutation) {
        case 0:
            putLittleEndian(std::span { invalid }.first(4), 0);
            break;
        case 1:
            putLittleEndian(std::span { invalid }.first(4), 2);
            break;
        case 2:
            putLittleEndian(std::span { invalid }.first(4), UINT32_MAX);
            break;
        case 3:
            ++invalid[0];
            break;
        case 4:
            invalid[requestKindOffset] = 3;
            break;
        case 5:
            invalid[requestKindOffset] = 255;
            break;
        case 6:
            invalid[requestKindOffset] = std::to_underlying(LocalClipboardKind::Types);
            break;
        case 7:
            invalid[requestKindOffset] = std::to_underlying(LocalClipboardKind::Route);
            break;
        case 8:
            putLittleEndian(std::span { invalid }.first(4), 3);
            invalid[requestMIMEOffset] = invalid[requestMIMEOffset + 1] = 0;
            break;
        }
        LocalClipboardRequestReader reader;
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        ScopedNothrowAllocationRefusal refuse;
        CHECK(reader.consume(invalid) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK(!reader.take());
        CHECK(reader.consume(body) == State::Refused);
    }
}

TEST(localClipboardRequestsBoundMIMEAndRefuseInvalidText)
{
    const std::string maximum(Sync::maxEffectClipboardMIMEBytes, 'm');
    const auto bytes = requestBytes(LocalClipboardKind::Item, maximum);
    REQUIRE(bytes.size() == localControlRequestBytes + requestHeaderBytes + maximum.size());
    for (const auto split : { localControlRequestBytes, localControlRequestBytes + requestHeaderBytes, bytes.size() - 1, bytes.size() }) {
        RequestExchange exchange;
        CHECK(exchange.consume(std::span { bytes }.first(split)) == (split == bytes.size() ? State::Ready : State::Awaiting));
        CHECK(exchange.consume(std::span { bytes }.subspan(split)) == State::Ready);
        const auto request = exchange.body.take();
        REQUIRE(request);
        CHECK(request->mime == maximum);
    }
    const std::string tooLong(maximum.size() + 1, 'm');
    const std::array invalidTexts { std::string("a\0b", 3), std::string("\x80", 1), std::string("\xc0\xaf", 2),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2) };
    for (const auto& mime : invalidTexts) {
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        CHECK(!encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Item, mime }));
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        auto invalid = requestBytes(LocalClipboardKind::Item, std::string(mime.size(), 'm'));
        std::ranges::copy(mime, invalid.begin() + localControlRequestBytes + requestHeaderBytes);
        RequestExchange exchange;
        CHECK(exchange.consume(invalid) == State::Refused);
        CHECK(!exchange.body.take());
    }
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(!encodeLocalClipboardRequest(0, { LocalClipboardKind::Types, {} }));
    CHECK(!encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Item, {} }));
    CHECK(!encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Types, "text/plain" }));
    CHECK(!encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Route, "text/plain" }));
    CHECK(!encodeLocalClipboardRequest(correlation, { static_cast<LocalClipboardKind>(3), {} }));
    CHECK(!encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Item, tooLong }));
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
}

TEST(localClipboardRequestRetainsBorrowedMIMEAndReportsAllocationFailure)
{
    const std::string mime(65'535, 'm');
    const auto bytes = requestBytes(LocalClipboardKind::Item, mime);
    REQUIRE(!bytes.empty());
    const auto body = std::span { bytes }.subspan(localControlRequestBytes);
    LocalClipboardRequestReader reader;
    for (std::size_t index = 0; index < requestHeaderBytes - 1; ++index) {
        CHECK(reader.consume(body.subspan(index, 1)) == State::Awaiting);
        CHECK_EQ(reader.retainedBytes(), index + 1);
    }
    CHECK(reader.consume(body.subspan(requestHeaderBytes - 1, 1)) == State::Awaiting);
    CHECK(reader.hasPendingRequest());
    CHECK_EQ(reader.retainedBytes(), requestHeaderBytes + mime.size());
    CHECK(reader.consume(body.subspan(requestHeaderBytes)) == State::Ready);
    const auto borrowed = reader.take();
    REQUIRE(borrowed);
    CHECK(!reader.hasPendingRequest());
    CHECK_EQ(reader.retainedBytes(), requestHeaderBytes + mime.size());
    CHECK(borrowed->mime == mime);
    CHECK(reader.consume({}) == State::Consumed);
    CHECK(borrowed->mime == mime);

    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    {
        ScopedNothrowAllocationRefusal refuse;
        const auto failure = encodeLocalClipboardRequest(correlation, { LocalClipboardKind::Item, mime });
        REQUIRE(!failure);
        CHECK(failure.error() == LocalControlError::NoMemory);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 1);
        LocalClipboardRequestReader refused;
        CHECK(refused.consume(body) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 2);
        CHECK_EQ(refused.retainedBytes(), requestHeaderBytes);
        CHECK(!refused.take());
    }
    RequestExchange retry;
    CHECK(retry.consume(bytes) == State::Ready);
    const auto request = retry.body.take();
    REQUIRE(request);
    CHECK(request->mime == mime);
}

TEST(localClipboardRequestCapacityAdmissionPrecedesAllocationAndKeepsItsOwnerCharged)
{
    const std::string mime(65'535, 'm');
    const auto bytes = requestBytes(LocalClipboardKind::Item, mime);
    REQUIRE(!bytes.empty());
    const auto body = std::span { bytes }.subspan(localControlRequestBytes);
    LocalClipboardRequestReader refused;
    std::size_t callbacks = 0;
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    const std::function<bool(std::size_t)> deny = [&](std::size_t requested) {
        ++callbacks;
        CHECK_EQ(requested, mime.size());
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        return false;
    };
    {
        ScopedNothrowAllocationRefusal refuse;
        CHECK(refused.consume(body.first(requestHeaderBytes - 1), deny) == State::Awaiting);
        CHECK_EQ(callbacks, 0u);
        CHECK(refused.consume(body.subspan(requestHeaderBytes - 1), deny) == State::Refused);
        CHECK_EQ(callbacks, 1u);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK_EQ(refused.retainedBytes(), requestHeaderBytes);
        CHECK(!refused.take());
    }
    LocalClipboardRequestReader admitted;
    callbacks = 0;
    const std::function<bool(std::size_t)> allow = [&](std::size_t requested) {
        ++callbacks;
        CHECK_EQ(requested, mime.size());
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        return true;
    };
    CHECK(admitted.consume(body.first(requestHeaderBytes), allow) == State::Awaiting);
    CHECK_EQ(callbacks, 1u);
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 1);
    CHECK_EQ(admitted.retainedBytes(), requestHeaderBytes + mime.size());
    CHECK(admitted.consume(body.subspan(requestHeaderBytes, 1), allow) == State::Awaiting);
    CHECK(admitted.consume(body.subspan(requestHeaderBytes + 1), allow) == State::Ready);
    CHECK_EQ(callbacks, 1u);
    const auto request = admitted.take();
    REQUIRE(request);
    CHECK(request->mime == mime);
    CHECK(!admitted.hasPendingRequest());
    CHECK_EQ(admitted.retainedBytes(), requestHeaderBytes + mime.size());
}

TEST(localClipboardRepliesKeepSelectionStatusAndPayloadDistinct)
{
    BinaryClipboard fixture;
    for (const bool host : { false, true }) {
        for (const auto status : { Sync::EffectClipboardStatus::OK, Sync::EffectClipboardStatus::Empty,
                 Sync::EffectClipboardStatus::Refused, Sync::EffectClipboardStatus::TooLarge }) {
            for (const bool payload : { false, true }) {
                const LocalClipboardReply reply { correlation, daemon, status, host, payload ? fixture.bundle() : Sync::EffectClipboardBundle {} };
                const bool valid = host ? status == Sync::EffectClipboardStatus::Empty && !payload
                                        : status == Sync::EffectClipboardStatus::OK || !payload;
                const auto size = localClipboardReplyBytes(reply);
                CHECK(size.has_value() == valid);
                if (!valid) {
                    const bool hasCanonicalPayload = status == Sync::EffectClipboardStatus::OK || payload;
                    auto invalid = replyBytes({ correlation, daemon,
                        hasCanonicalPayload ? Sync::EffectClipboardStatus::OK : Sync::EffectClipboardStatus::Empty,
                        false, payload ? fixture.bundle() : Sync::EffectClipboardBundle {} });
                    REQUIRE(invalid.size() >= localClipboardReplyHeaderBytes);
                    invalid[replyStatusOffset] = std::to_underlying(status);
                    invalid[replySelectionOffset] = static_cast<std::uint8_t>(host);
                    LocalClipboardReplyReader reader(correlation);
                    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
                    ScopedNothrowAllocationRefusal refuse;
                    CHECK(reader.consume(invalid) == State::Refused);
                    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
                    CHECK(!reader.take());
                    continue;
                }
                const auto bytes = replyBytes(reply);
                REQUIRE(!bytes.empty());
                CHECK((bytes.size() > localClipboardReplyHeaderBytes) == (status == Sync::EffectClipboardStatus::OK));
                LocalClipboardReplyReader reader(correlation);
                CHECK(reader.consume(bytes) == State::Ready);
                const auto decoded = reader.take();
                REQUIRE(decoded);
                checkReply(*decoded, reply);
            }
        }
    }
    for (const auto& invalid : { LocalClipboardReply { 0, daemon }, LocalClipboardReply { correlation, 0 },
             LocalClipboardReply { correlation, daemon, static_cast<Sync::EffectClipboardStatus>(255) } })
        CHECK(!localClipboardReplyBytes(invalid));
}

TEST(localClipboardReplyHeadersValidateIdentityAndBoundsBeforeAllocation)
{
    BinaryClipboard fixture;
    const auto bytes = replyBytes({ correlation, daemon, Sync::EffectClipboardStatus::OK, false, fixture.bundle() });
    REQUIRE(bytes.size() > localClipboardReplyHeaderBytes);
    for (std::size_t split = 0; split < localClipboardReplyHeaderBytes; ++split) {
        LocalClipboardReplyReader reader(correlation);
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        ScopedNothrowAllocationRefusal refuse;
        CHECK(reader.consume(std::span { bytes }.first(split)) == State::Awaiting);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK(reader.consume(std::span { bytes }.subspan(split, localClipboardReplyHeaderBytes - split)) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 1);
        CHECK(!reader.take());
    }
    for (std::size_t mutation = 0; mutation < localControlPreamble.size() + 10; ++mutation) {
        auto invalid = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + localClipboardReplyHeaderBytes);
        if (mutation < localControlPreamble.size())
            invalid[mutation] ^= 0xff;
        else {
            switch (mutation - localControlPreamble.size()) {
            case 0:
                invalid[localControlPreamble.size()] = localReloadReplyOperation;
                break;
            case 1:
                invalid[replyStatusOffset] = 255;
                break;
            case 2:
                invalid[replySelectionOffset] = 2;
                break;
            case 3:
                putLittleEndian(std::span { invalid }.subspan(replyCorrelationOffset, 8), 0);
                break;
            case 4:
                putLittleEndian(std::span { invalid }.subspan(replyCorrelationOffset, 8), correlation + 1);
                break;
            case 5:
                putLittleEndian(std::span { invalid }.subspan(replyDaemonOffset, 8), 0);
                break;
            case 6:
                putLittleEndian(std::span { invalid }.subspan(replyLengthOffset, 4), Sync::maxEffectClipboardEncodedReadBytes + 1);
                break;
            case 7:
                putLittleEndian(std::span { invalid }.subspan(replyLengthOffset, 4), 0);
                break;
            case 8:
                invalid[replySelectionOffset] = 1;
                break;
            case 9:
                invalid[replyStatusOffset] = std::to_underlying(Sync::EffectClipboardStatus::Empty);
                break;
            }
        }
        LocalClipboardReplyReader reader(correlation);
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        ScopedNothrowAllocationRefusal refuse;
        CHECK(reader.consume(invalid) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK(!reader.take());
    }
    LocalClipboardReplyReader zero(0);
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(zero.consume(bytes) == State::Refused);
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
}

TEST(localClipboardRepliesSurviveEverySplitAndRejectTrailingPackets)
{
    BinaryClipboard fixture;
    const std::array replies { LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::Empty, true },
        LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, fixture.bundle() },
        LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::OK },
        LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::Empty },
        LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::Refused },
        LocalClipboardReply { correlation, daemon, Sync::EffectClipboardStatus::TooLarge } };
    for (const auto& reply : replies) {
        const auto bytes = replyBytes(reply);
        REQUIRE(!bytes.empty());
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            LocalClipboardReplyReader reader(correlation);
            CHECK(reader.consume(std::span { bytes }.first(split)) == (split == bytes.size() ? State::Ready : State::Awaiting));
            if (split < bytes.size())
                CHECK(!reader.take());
            CHECK(reader.consume(std::span { bytes }.subspan(split)) == State::Ready);
            const auto decoded = reader.take();
            REQUIRE(decoded);
            checkReply(*decoded, reply);
            CHECK(!reader.take());
            CHECK(reader.consume({}) == State::Consumed);
            const std::uint8_t extra = 0;
            CHECK(reader.consume({ &extra, 1 }) == State::Refused);
            CHECK(!reader.take());
        }
        LocalClipboardReplyReader bytewise(correlation);
        for (std::size_t index = 0; index < bytes.size(); ++index)
            CHECK(bytewise.consume(std::span { bytes }.subspan(index, 1)) == (index + 1 == bytes.size() ? State::Ready : State::Awaiting));
        CHECK(bytewise.take().has_value());
        auto trailing = bytes;
        trailing.push_back(0);
        LocalClipboardReplyReader coalesced(correlation);
        CHECK(coalesced.consume(trailing) == State::Refused);
        CHECK(!coalesced.take());
        LocalClipboardReplyReader later(correlation);
        CHECK(later.consume(bytes) == State::Ready);
        CHECK(later.consume(bytes) == State::Refused);
        CHECK(!later.take());
    }
}

TEST(localClipboardRepliesUseOnlyTheCanonicalBinaryBundle)
{
    std::array<std::uint8_t, 256> binary {};
    for (std::size_t index = 0; index < binary.size(); ++index)
        binary[index] = static_cast<std::uint8_t>(index);
    Sync::EffectClipboardBundle bundle {};
    bundle.count = 2;
    bundle.items[0] = { "application/octet-stream", binary };
    bundle.items[1] = { "text/plain", {} };
    const auto canonical = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardReadBytes);
    REQUIRE(canonical);
    const LocalClipboardReply reply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, bundle };
    const auto bytes = replyBytes(reply);
    REQUIRE(bytes.size() == localClipboardReplyHeaderBytes + canonical->size);
    CHECK(std::ranges::equal(std::span { bytes }.subspan(localClipboardReplyHeaderBytes), canonical->view()));
    LocalClipboardReplyReader reader(correlation);
    CHECK(reader.consume(bytes) == State::Ready);
    const auto decoded = reader.take();
    REQUIRE(decoded);
    checkReply(*decoded, reply);

    // These are malformed canonical bundles, not a second clipboard grammar.
    for (const auto& payload : { std::vector<std::uint8_t> { 0, 0 }, std::vector<std::uint8_t> { 33 },
             std::vector<std::uint8_t> { 1, 0, 0 }, std::vector<std::uint8_t> { 1, 1, 'm' } }) {
        auto invalid = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + localClipboardReplyHeaderBytes);
        putLittleEndian(std::span { invalid }.subspan(replyLengthOffset, 4), payload.size());
        invalid.insert(invalid.end(), payload.begin(), payload.end());
        LocalClipboardReplyReader malformed(correlation);
        CHECK(malformed.consume(invalid) == State::Refused);
        CHECK(!malformed.take());
    }
}

TEST(localClipboardRepliesAdmitMaximumCanonicalMetadataAndData)
{
    std::array<std::string, Sync::maxEffectClipboardItems> names;
    std::vector<std::uint8_t> data(Sync::maxEffectClipboardReadBytes + 1, 0x8a);
    Sync::EffectClipboardBundle bundle {};
    bundle.count = names.size();
    for (std::size_t index = 0; index < names.size(); ++index) {
        names[index] = std::string(Sync::maxEffectClipboardMIMEBytes, 'm');
        names[index][0] = static_cast<char>('A' + index);
        bundle.items[index] = { names[index], {} };
    }
    bundle.items[0].data = std::span { data }.first(Sync::maxEffectClipboardReadBytes);
    const auto canonical = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardReadBytes);
    REQUIRE(canonical);
    CHECK(canonical->size <= Sync::maxEffectClipboardEncodedReadBytes);
    CHECK(canonical->size > Sync::maxEffectClipboardReadBytes + 65'536);
    const LocalClipboardReply reply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, bundle };
    const auto bytes = replyBytes(reply);
    REQUIRE(bytes.size() == localClipboardReplyHeaderBytes + canonical->size);
    CHECK(std::ranges::equal(std::span { bytes }.subspan(localClipboardReplyHeaderBytes), canonical->view()));
    for (const auto split : { localClipboardReplyHeaderBytes - 1, localClipboardReplyHeaderBytes, bytes.size() - 1, bytes.size() }) {
        LocalClipboardReplyReader reader(correlation);
        CHECK(reader.consume(std::span { bytes }.first(split)) == (split == bytes.size() ? State::Ready : State::Awaiting));
        CHECK(reader.consume(std::span { bytes }.subspan(split)) == State::Ready);
        const auto decoded = reader.take();
        REQUIRE(decoded);
        checkReply(*decoded, reply);
    }
    auto invalid = reply;
    const std::string tooLong(Sync::maxEffectClipboardMIMEBytes + 1, 'm');
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    invalid.bundle.items[0].data = data;
    CHECK(!localClipboardReplyBytes(invalid));
    invalid = reply;
    invalid.bundle.items[1].data = std::span { data }.first(1);
    CHECK(!localClipboardReplyBytes(invalid));
    invalid = reply;
    invalid.bundle.items[0].mime = tooLong;
    CHECK(!localClipboardReplyBytes(invalid));
    invalid = reply;
    invalid.bundle.items[1].mime = invalid.bundle.items[0].mime;
    CHECK(!localClipboardReplyBytes(invalid));
    invalid = reply;
    invalid.bundle.count = Sync::maxEffectClipboardItems + 1;
    CHECK(!localClipboardReplyBytes(invalid));
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
}

TEST(localClipboardReplyAllocationFailureNeverPublishesPartialPayload)
{
    BinaryClipboard fixture;
    const LocalClipboardReply reply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, fixture.bundle() };
    const auto bytes = replyBytes(reply);
    REQUIRE(bytes.size() > localClipboardReplyHeaderBytes);
    LocalClipboardReplyReader reader(correlation);
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    {
        ScopedNothrowAllocationRefusal refuse;
        CHECK(reader.consume(bytes) == State::Refused);
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts + 1);
        CHECK(!reader.take());
    }
    CHECK(reader.consume(bytes) == State::Refused);
    CHECK(!reader.take());
    LocalClipboardReplyReader retry(correlation);
    CHECK(retry.consume(bytes) == State::Ready);
    const auto decoded = retry.take();
    REQUIRE(decoded);
    checkReply(*decoded, reply);
}

TEST(localClipboardWritersRefuseBeforeChangingMismatchedStorage)
{
    BinaryClipboard fixture;
    const auto bundle = fixture.bundle();
    const LocalClipboardReply reply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, bundle };
    const auto size = localClipboardReplyBytes(reply);
    const auto payloadSize = Sync::effectClipboardEncodedBytes(bundle, Sync::maxEffectClipboardReadBytes);
    REQUIRE(size);
    REQUIRE(payloadSize);
    for (const auto length : { std::size_t { 0 }, *size - 1, *size + 1 }) {
        std::vector<std::uint8_t> output(length, 0x5a);
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        CHECK(!writeLocalClipboardReply(reply, output));
        CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x5a; }));
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    }
    for (const auto length : { std::size_t { 0 }, *payloadSize - 1, *payloadSize + 1 }) {
        std::vector<std::uint8_t> output(length, 0xa5);
        const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
        CHECK(!Sync::writeEffectClipboard(bundle, Sync::maxEffectClipboardReadBytes, output));
        CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0xa5; }));
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    }
    std::vector<std::uint8_t> output(*size, 0x5a);
    auto invalid = reply;
    invalid.correlation = 0;
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(!writeLocalClipboardReply(invalid, output));
    CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x5a; }));
    invalid = reply;
    invalid.bundle.items[0].mime = {};
    CHECK(!writeLocalClipboardReply(invalid, output));
    CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x5a; }));
    std::vector<std::uint8_t> payload(*payloadSize, 0xa5);
    CHECK(!Sync::writeEffectClipboard(invalid.bundle, Sync::maxEffectClipboardReadBytes, payload));
    CHECK(std::ranges::all_of(payload, [](auto byte) { return byte == 0xa5; }));
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    {
        ScopedNothrowAllocationRefusal refuse;
        CHECK(writeLocalClipboardReply(reply, output).has_value());
        CHECK(Sync::writeEffectClipboard(bundle, Sync::maxEffectClipboardReadBytes, payload).has_value());
        CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    }
}

TEST(localClipboardRequestsPreserveBoundedMIMEBytesAcrossPartitions)
{
    for (const std::size_t length : std::array<std::size_t, 3> { 25, Sync::maxEffectClipboardMIMEBytes - 1, Sync::maxEffectClipboardMIMEBytes }) {
        const std::string mime = "application/" + std::string(length - 12, 'x');
        const auto bytes = requestBytes(LocalClipboardKind::Item, mime);
        REQUIRE(!bytes.empty());
        CHECK(UbiquittyTest::sweepStreamPartitions(bytes.size(), length == 25 ? UbiquittyTest::PartitionCoverage::Exhaustive : UbiquittyTest::PartitionCoverage::Boundaries, [&](auto ends, const std::string& label) {
            RequestExchange exchange;
            UbiquittyTest::feedStreamPartition(std::span<const std::uint8_t>(bytes), ends, [&](auto piece) { (void)exchange.consume(piece); });
            const auto request = exchange.body.take();
            const bool matches = exchange.operation && exchange.operation->correlation == correlation
                && request && request->kind == LocalClipboardKind::Item && request->mime == mime;
            CHECK_MSG(matches, ("MIME=" + std::to_string(length) + " " + label).c_str());
            return matches;
        }));
    }
}

TEST(localClipboardRepliesPreserveBinaryBytesAtTheReadBoundAcrossPartitions)
{
    for (const std::size_t length : std::array<std::size_t, 2> { 257, Sync::maxEffectClipboardReadBytes }) {
        std::vector<std::uint8_t> data(length);
        for (std::size_t index = 0; index < data.size(); ++index)
            data[index] = static_cast<std::uint8_t>((index * 37 + index / 251) % 256);
        Sync::EffectClipboardBundle bundle;
        bundle.count = 1;
        bundle.items[0] = { "application/octet-stream", data };
        const LocalClipboardReply reply { correlation, daemon, Sync::EffectClipboardStatus::OK, false, bundle };
        const auto bytes = replyBytes(reply);
        REQUIRE(!bytes.empty());
        CHECK(UbiquittyTest::sweepStreamPartitions(bytes.size(), length == 257 ? UbiquittyTest::PartitionCoverage::Exhaustive : UbiquittyTest::PartitionCoverage::Boundaries, [&](auto ends, const std::string& label) {
            LocalClipboardReplyReader reader(correlation);
            UbiquittyTest::feedStreamPartition(std::span<const std::uint8_t>(bytes), ends, [&](auto piece) { (void)reader.consume(piece); });
            const auto actual = reader.take();
            const bool matches = actual && actual->correlation == correlation && actual->daemonInstanceID == daemon
                && actual->status == reply.status && actual->isHostSelected == reply.isHostSelected && actual->bundle.count == 1
                && actual->bundle.items[0].mime == bundle.items[0].mime && std::ranges::equal(actual->bundle.items[0].data, data);
            CHECK_MSG(matches, ("data=" + std::to_string(length) + " " + label).c_str());
            return matches;
        }));
    }
}

} // namespace

UBIQUITTY_TEST_MAIN()
