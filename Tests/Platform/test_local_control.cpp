// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Platform/Host/LocalControl.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace Ubiquitty::Host;

TEST(localReloadRequestHasFrozenBoundedBytes)
{
    const auto request = encodeLocalReloadRequest(0x0807060504030201);
    REQUIRE(request.size() == 25);
    CHECK(std::ranges::equal(std::span { request }.first(localControlPreamble.size()), localControlPreamble));
    CHECK_EQ(request[16], 1u);
    for (std::size_t index = 0; index < 8; ++index)
        CHECK_EQ(request[17 + index], index + 1);
}

TEST(localReloadRequestSurvivesEveryPacketBoundary)
{
    const auto request = encodeLocalReloadRequest(0x0807060504030201);
    for (std::size_t split = 0; split <= request.size(); ++split) {
        LocalControlReader reader;
        CHECK(reader.consume(std::span { request }.first(split))
            == (split == request.size() ? LocalControlReader::State::Ready : LocalControlReader::State::Awaiting));
        CHECK_EQ(reader.pendingBytes(), split);
        CHECK(reader.hasPendingRequest() == (split > 0 && split < request.size()));
        if (split < request.size())
            CHECK(!reader.take());
        CHECK(reader.consume(std::span { request }.subspan(split)) == LocalControlReader::State::Ready);
        CHECK(!reader.hasPendingRequest());
        const auto correlation = reader.take();
        REQUIRE(correlation);
        CHECK(correlation->operation == Ubiquitty::Protocol::Local::LocalControlOperation::ReloadCredentials);
        CHECK_EQ(correlation->correlation, 0x0807060504030201ull);
        CHECK(!reader.take());
        CHECK(reader.consume(request) == LocalControlReader::State::Consumed);
        CHECK_EQ(reader.pendingBytes(), localControlRequestBytes);
        CHECK(!reader.hasPendingRequest());
    }
    LocalControlReader reader;
    for (std::size_t index = 0; index < request.size(); ++index) {
        CHECK(reader.consume(std::span { request }.subspan(index, 1))
            == (index + 1 == request.size() ? LocalControlReader::State::Ready : LocalControlReader::State::Awaiting));
    }
    CHECK(reader.take().has_value());
}

TEST(ordinaryFirstBytesPermanentlyBypassLocalControlWithoutBeingConsumed)
{
    const auto request = encodeLocalReloadRequest(9);
    for (const auto first : { std::uint8_t { 1 }, std::uint8_t { 0x0e }, std::uint8_t { 0xf8 } }) {
        LocalControlReader reader;
        CHECK(reader.consume(std::span { &first, 1 }) == LocalControlReader::State::Ordinary);
        CHECK_EQ(reader.pendingBytes(), 0u);
        CHECK(!reader.hasPendingRequest());
        CHECK(reader.consume(request) == LocalControlReader::State::Ordinary);
        CHECK(!reader.take());
    }
}

TEST(malformedLocalRequestsNeverYieldAReload)
{
    const auto request = encodeLocalReloadRequest(9);
    for (std::size_t index = 1; index <= localControlPreamble.size(); ++index) {
        auto malformed = request;
        malformed[index] ^= 0xff;
        LocalControlReader reader;
        CHECK(reader.consume(malformed) == LocalControlReader::State::Refused);
        CHECK(!reader.take());
        CHECK(reader.consume(request) == LocalControlReader::State::Refused);
    }
    LocalControlReader zero;
    CHECK(zero.consume(encodeLocalReloadRequest(0)) == LocalControlReader::State::Refused);
    CHECK(!zero.take());
    for (const auto extra : { std::size_t { 1 }, localControlRequestBytes, std::size_t { 65'536 } }) {
        std::vector<std::uint8_t> oversized(request.begin(), request.end());
        oversized.resize(request.size() + extra);
        LocalControlReader reader;
        CHECK(reader.consume(oversized) == LocalControlReader::State::Refused);
        CHECK_EQ(reader.pendingBytes(), 0u);
        CHECK(!reader.take());
    }
    LocalControlReader partial;
    CHECK(partial.consume(std::span { request }.first(1)) == LocalControlReader::State::Awaiting);
    CHECK(partial.hasPendingRequest());
    CHECK(partial.consume(request) == LocalControlReader::State::Refused);
    CHECK(!partial.hasPendingRequest());
    CHECK(!partial.take());
}

TEST(onlyAnAuthenticatedOwnersFirstLocalRequestReloadsCredentials)
{
    const LocalControlPeer owner { .isSameUserLocalStream = true, .isTrustedOwnerBinary = true };
    for (unsigned refused = 0; refused < 4; ++refused) {
        auto peer = owner;
        switch (refused) {
        case 0:
            peer.isSameUserLocalStream = false;
            break;
        case 1:
            peer.isTrustedOwnerBinary = false;
            break;
        case 2:
            peer.hasRemoteGreeting = true;
            break;
        case 3:
            peer.hasDescriptors = true;
            break;
        }
        unsigned reloads = 0;
        const auto reply = handleLocalReload({ .correlation = 19 }, 27, peer, [&] { ++reloads; });
        CHECK_EQ(reloads, 0u);
        CHECK(reply.status == LocalControlStatus::Refused);
        CHECK_EQ(reply.correlation, 19u);
        CHECK_EQ(reply.daemonInstanceID, 27u);
    }
    unsigned reloads = 0;
    const auto reply = handleLocalReload({ .correlation = 19 }, 27, owner, [&] { ++reloads; });
    CHECK_EQ(reloads, 1u);
    CHECK(reply.status == LocalControlStatus::Reloaded);
    CHECK(handleLocalReload({ .correlation = 0 }, 27, owner, [&] { ++reloads; }).status == LocalControlStatus::Refused);
    CHECK(handleLocalReload({ .correlation = 19 }, 0, owner, [&] { ++reloads; }).status == LocalControlStatus::Refused);
    CHECK_EQ(reloads, 1u);
}

TEST(aDifferentSelectedLocalOperationCannotReloadCredentialStores)
{
    using Operation = Ubiquitty::Protocol::Local::LocalControlOperation;
    const LocalControlPeer owner { .isSameUserLocalStream = true, .isTrustedOwnerBinary = true };
    unsigned reloads = 0;
    for (const auto operation : { Operation::QUICToken, Operation::SessionHandoff, Operation::Clipboard,
             Operation::SessionReclaim, static_cast<Operation>(0), static_cast<Operation>(255) }) {
        const auto reply = handleLocalReload({ .operation = operation, .correlation = 19 }, 27, owner, [&] { ++reloads; });
        CHECK(reply.status == LocalControlStatus::Refused);
        CHECK_EQ(reply.correlation, 19u);
        CHECK_EQ(reply.daemonInstanceID, 27u);
        CHECK_EQ(reloads, 0u);
    }
}

TEST(localReclaimSelectionIsExactAcrossEveryPacketBoundary)
{
    namespace Local = Ubiquitty::Protocol::Local;
    const auto encoded = Local::encodeLocalReclaimRequest(0x0807060504030201);
    REQUIRE(encoded);
    CHECK_EQ((*encoded)[localControlPreamble.size()], 5u);
    for (std::size_t split = 0; split <= encoded->size(); ++split) {
        LocalControlReader reader;
        CHECK(reader.consume(std::span(*encoded).first(split))
            == (split == encoded->size() ? LocalControlReader::State::Ready : LocalControlReader::State::Awaiting));
        CHECK(reader.consume(std::span(*encoded).subspan(split)) == LocalControlReader::State::Ready);
        const auto request = reader.take();
        REQUIRE(request);
        CHECK(request->operation == Local::LocalControlOperation::SessionReclaim);
        CHECK_EQ(request->correlation, 0x0807060504030201ull);
        CHECK(!reader.take());
        CHECK(reader.consume(*encoded) == LocalControlReader::State::Consumed);
    }
    CHECK(!Local::encodeLocalReclaimRequest(0));
    auto zero = *encoded;
    std::fill(zero.end() - sizeof(std::uint64_t), zero.end(), 0);
    LocalControlReader zeroReader;
    CHECK(zeroReader.consume(zero) == LocalControlReader::State::Refused);
    CHECK(!zeroReader.take());
    for (std::size_t split = 0; split < encoded->size(); ++split) {
        std::vector<std::uint8_t> trailing(encoded->begin(), encoded->end());
        trailing.push_back(0);
        LocalControlReader reader;
        CHECK(reader.consume(std::span(trailing).first(split)) == LocalControlReader::State::Awaiting);
        CHECK(reader.consume(std::span(trailing).subspan(split)) == LocalControlReader::State::Refused);
        CHECK(!reader.take());
    }
}

// The relay mark is the one first-packet operation a connection outlives: the
// bridge sends the client's own stream straight after it, sometimes in the same
// packet, and the reader hands every later byte to the ordinary carrier.
TEST(aRemoteRelayMarkHandsTheRestOfTheConnectionToTheCarrier)
{
    namespace Local = Ubiquitty::Protocol::Local;
    const auto encoded = Local::encodeLocalRemoteRelayRequest(1);
    REQUIRE(encoded);
    CHECK_EQ((*encoded)[localControlPreamble.size()], 6u);
    CHECK(!Local::encodeLocalRemoteRelayRequest(0));
    const std::array<std::uint8_t, 3> clientBytes { 0x0e, 0x01, 0x02 };
    for (std::size_t split = 0; split <= encoded->size(); ++split) {
        std::vector<std::uint8_t> packet(encoded->begin(), encoded->end());
        packet.insert(packet.end(), clientBytes.begin(), clientBytes.end());
        LocalControlReader reader;
        const auto first = reader.consume(std::span(packet).first(split));
        CHECK(first == (split == encoded->size() ? LocalControlReader::State::Ready : LocalControlReader::State::Awaiting));
        if (first == LocalControlReader::State::Awaiting) {
            // The rest of the mark and the client's bytes arrive together.
            CHECK(reader.consume(std::span(packet).subspan(split)) == LocalControlReader::State::Ready);
            CHECK_EQ(reader.consumedBytes(), encoded->size() - split);
        }
        const auto request = reader.take();
        REQUIRE(request);
        CHECK(request->operation == Local::LocalControlOperation::RemoteRelay);
        reader.continueAsCarrier();
        CHECK(reader.consume(clientBytes) == LocalControlReader::State::Ordinary);
        CHECK(!reader.take());
    }
}

// Only a consumed relay mark hands the connection on; any other operation, or
// none, stays the one request it was.
TEST(onlyARelayMarkContinuesAsACarrier)
{
    LocalControlReader reload;
    CHECK(reload.consume(encodeLocalReloadRequest(3)) == LocalControlReader::State::Ready);
    REQUIRE(reload.take());
    reload.continueAsCarrier();
    CHECK(reload.consume(std::array<std::uint8_t, 1> { 0x0e }) == LocalControlReader::State::Refused);

    LocalControlReader untaken;
    const auto mark = Ubiquitty::Protocol::Local::encodeLocalRemoteRelayRequest(1);
    REQUIRE(mark);
    CHECK(untaken.consume(*mark) == LocalControlReader::State::Ready);
    untaken.continueAsCarrier();
    CHECK(untaken.consume(std::array<std::uint8_t, 1> { 0x0e }) == LocalControlReader::State::Refused);
}

// The daemon acknowledges a relay mark with one fixed reply that repeats the
// request's correlation and names whether the mark was applied. It names its
// instance only when it applied the mark, that is to its own bridge. A bridge
// reads exactly that many bytes before the carrier's own, so anything else in
// that place is refused rather than read as an acknowledgement.
TEST(aRemoteRelayAcknowledgementIsExactAndRepeatsTheMark)
{
    using namespace Ubiquitty::Protocol::Local;
    for (const bool isMarked : { true, false }) {
        const LocalRemoteRelayReply reply { .correlation = 1, .daemonInstanceID = isMarked ? 27u : 0u, .isMarked = isMarked };
        const auto encoded = encodeLocalRemoteRelayReply(reply);
        REQUIRE(encoded);
        CHECK_EQ(encoded->size(), localControlReplyBytes);
        const auto decoded = decodeLocalRemoteRelayReply(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == reply);
        CHECK(!decodeLocalRemoteRelayReply(std::span(*encoded).first(encoded->size() - 1)));
        auto wrongOperation = *encoded;
        wrongOperation[localControlPreamble.size()] = localReloadReplyOperation;
        CHECK(!decodeLocalRemoteRelayReply(wrongOperation));
        auto wrongStatus = *encoded;
        wrongStatus[localControlPreamble.size() + 1] = 2;
        CHECK(!decodeLocalRemoteRelayReply(wrongStatus));
        // The status and the instance agree or the reply is refused.
        auto flipped = *encoded;
        flipped[localControlPreamble.size() + 1] = isMarked ? 0 : 1;
        CHECK(!decodeLocalRemoteRelayReply(flipped));
    }
    CHECK(!encodeLocalRemoteRelayReply({ .correlation = 0, .daemonInstanceID = 27, .isMarked = true }));
    CHECK(!encodeLocalRemoteRelayReply({ .correlation = 1, .daemonInstanceID = 0, .isMarked = true }));
    CHECK_MSG(!encodeLocalRemoteRelayReply({ .correlation = 1, .daemonInstanceID = 27, .isMarked = false }), "an acknowledgement named the instance to a peer it did not mark");
    // A reload reply is not an acknowledgement, though it is the same size.
    CHECK(!decodeLocalRemoteRelayReply(encodeLocalControlReply({ .correlation = 1, .daemonInstanceID = 27, .status = LocalControlStatus::Reloaded })));
}

TEST(localReloadRepliesKeepTheirExactRequestAndDaemonIdentity)
{
    for (const auto status : { LocalControlStatus::Reloaded, LocalControlStatus::Refused }) {
        const LocalControlReply reply { .correlation = 0x0807060504030201, .daemonInstanceID = 0x1817161514131211, .status = status };
        const auto bytes = encodeLocalControlReply(reply);
        REQUIRE(bytes.size() == 34);
        const auto decoded = decodeLocalControlReply(bytes);
        REQUIRE(decoded);
        CHECK(*decoded == reply);
        for (std::size_t length = 0; length < bytes.size(); ++length)
            CHECK(!decodeLocalControlReply(std::span { bytes }.first(length)));
        for (std::size_t index = 0; index < localControlPreamble.size() + 1; ++index) {
            auto malformed = bytes;
            malformed[index] ^= 0xff;
            CHECK(!decodeLocalControlReply(malformed));
        }
        auto unknownStatus = bytes;
        unknownStatus[localControlPreamble.size() + 1] = 2;
        CHECK(!decodeLocalControlReply(unknownStatus));
        std::vector<std::uint8_t> trailing(bytes.begin(), bytes.end());
        trailing.push_back(0);
        CHECK(!decodeLocalControlReply(trailing));
    }
    CHECK(!decodeLocalControlReply(encodeLocalControlReply({ .correlation = 0, .daemonInstanceID = 27 })));
    CHECK(!decodeLocalControlReply(encodeLocalControlReply({ .correlation = 19, .daemonInstanceID = 0 })));
}

UBIQUITTY_TEST_MAIN()
