// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Local/Control.h"
#include "Platform/Host/LocalControl.h"

#include <limits>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Local;

namespace {

LocalQUICTokenReply grant(std::size_t tokenLength = 32)
{
    LocalQUICTokenReply reply { .correlation = 19, .daemonInstanceID = 27, .status = LocalQUICTokenStatus::Granted, .port = 4321, .tokenLength = static_cast<std::uint8_t>(tokenLength) };
    for (std::size_t index = 0; index < std::min(tokenLength, reply.token.size()); ++index)
        reply.token[index] = static_cast<std::uint8_t>(index * 7);
    for (std::size_t index = 0; index < reply.certificateSHA256.size(); ++index)
        reply.certificateSHA256[index] = static_cast<std::uint8_t>(255 - index);
    return reply;
}

} // namespace

TEST(localQUICTokenRequestsUseOnlyTheFrozenPreambleOperationAndCorrelation)
{
    const auto request = encodeLocalQUICTokenRequest(0x0807060504030201);
    REQUIRE(request);
    CHECK_EQ(request->size(), 25u);
    CHECK(std::ranges::equal(std::span { *request }.first(localControlPreamble.size()), localControlPreamble));
    CHECK_EQ((*request)[16], 2u);
    for (std::size_t index = 0; index < 8; ++index)
        CHECK_EQ((*request)[17 + index], index + 1);
    auto reload = encodeLocalReloadRequest(0x0807060504030201);
    CHECK_EQ(reload[16], 1u);
    reload[16] = 2;
    CHECK(reload == *request);
    CHECK(!encodeLocalQUICTokenRequest(0));
}

TEST(bothLocalOperationsShareOneFixedFirstPacketReader)
{
    for (const auto operation : { LocalControlOperation::ReloadCredentials, LocalControlOperation::QUICToken }) {
        auto request = encodeLocalReloadRequest(19);
        request[16] = std::to_underlying(operation);
        for (std::size_t split = 0; split <= request.size(); ++split) {
            LocalControlReader reader;
            CHECK(reader.consume(std::span { request }.first(split))
                == (split == request.size() ? LocalControlReader::State::Ready : LocalControlReader::State::Awaiting));
            CHECK(reader.hasPendingRequest() == (split && split < request.size()));
            CHECK(reader.consume(std::span { request }.subspan(split)) == LocalControlReader::State::Ready);
            const auto decoded = reader.take();
            REQUIRE(decoded);
            CHECK(decoded->operation == operation);
            CHECK_EQ(decoded->correlation, 19u);
            CHECK(!reader.hasPendingRequest());
            CHECK_EQ(reader.pendingBytes(), 25u);
            CHECK(!reader.take());
            CHECK(reader.consume(request) == LocalControlReader::State::Consumed);
        }
        for (const auto invalidOperation : { 0u, 0x7Fu, 0x81u, 0x82u }) {
            auto invalid = request;
            invalid[16] = static_cast<std::uint8_t>(invalidOperation);
            LocalControlReader reader;
            CHECK(reader.consume(invalid) == LocalControlReader::State::Refused);
            CHECK(!reader.take());
        }
        std::vector<std::uint8_t> trailing(request.begin(), request.end());
        trailing.push_back(0);
        LocalControlReader oversized;
        CHECK(oversized.consume(trailing) == LocalControlReader::State::Refused);
        CHECK_EQ(oversized.pendingBytes(), 0u);
        auto zero = request;
        std::fill(zero.begin() + 17, zero.end(), 0);
        LocalControlReader uncorrelated;
        CHECK(uncorrelated.consume(zero) == LocalControlReader::State::Refused);
    }
}

TEST(localQUICTokenGrantsRoundTripOpaqueBinaryAtTheirExactBounds)
{
    for (const auto length : { std::size_t { 1 }, std::size_t { 32 }, maxQUICTokenLength }) {
        const auto reply = grant(length);
        const auto encoded = encodeLocalQUICTokenReply(reply);
        REQUIRE(encoded);
        CHECK_EQ(encoded->length, 69u + length);
        CHECK(encoded->length <= 133u);
        CHECK_EQ(encoded->bytes[16], 0x82u);
        CHECK_EQ(encoded->bytes[17], 0u);
        CHECK_EQ(encoded->bytes[34], 0xe1u);
        CHECK_EQ(encoded->bytes[35], 0x10u);
        CHECK_EQ(encoded->bytes[36], length);
        const auto decoded = decodeLocalQUICTokenReply(encoded->view());
        REQUIRE(decoded);
        CHECK(*decoded == reply);
    }
    for (const auto status : { LocalQUICTokenStatus::Disabled, LocalQUICTokenStatus::Refused }) {
        const LocalQUICTokenReply refused { .correlation = 19, .daemonInstanceID = 27, .status = status };
        const auto encoded = encodeLocalQUICTokenReply(refused);
        REQUIRE(encoded);
        CHECK_EQ(encoded->length, 34u);
        const auto decoded = decodeLocalQUICTokenReply(encoded->view());
        REQUIRE(decoded);
        CHECK(*decoded == refused);
    }
}

TEST(localQUICTokenCodecRefusesMalformedIdentityShapeAndSecretsOnRefusal)
{
    const auto valid = encodeLocalQUICTokenReply(grant());
    REQUIRE(valid);
    for (std::size_t length = 0; length < valid->length; ++length)
        CHECK(!decodeLocalQUICTokenReply(valid->view().first(length)));
    for (std::size_t index = 0; index < localControlPreamble.size() + 1; ++index) {
        auto malformed = *valid;
        malformed.bytes[index] ^= 0xff;
        CHECK(!decodeLocalQUICTokenReply(malformed.view()));
    }
    for (const auto index : { std::size_t { 18 }, std::size_t { 26 } }) {
        auto zeroIdentity = *valid;
        std::fill_n(zeroIdentity.bytes.begin() + index, 8, 0);
        CHECK(!decodeLocalQUICTokenReply(zeroIdentity.view()));
    }
    for (const auto tokenLength : { 0u, 65u, 255u }) {
        auto malformed = *valid;
        malformed.bytes[36] = static_cast<std::uint8_t>(tokenLength);
        CHECK(!decodeLocalQUICTokenReply(malformed.view()));
    }
    for (const auto extra : { std::size_t { 1 }, std::size_t { 133 }, std::size_t { 65'536 } }) {
        std::vector<std::uint8_t> trailing(valid->view().begin(), valid->view().end());
        trailing.resize(trailing.size() + extra);
        CHECK(!decodeLocalQUICTokenReply(trailing));
        LocalQUICTokenReplyReader reader(19);
        CHECK(reader.consume(trailing) == LocalQUICTokenReplyReader::State::Refused);
    }
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        auto reply = grant();
        switch (invalid) {
        case 0:
            reply.correlation = 0;
            break;
        case 1:
            reply.daemonInstanceID = 0;
            break;
        case 2:
            reply.port = 0;
            break;
        case 3:
            reply.tokenLength = 0;
            break;
        case 4:
            reply.tokenLength = 65;
            break;
        case 5:
            reply.status = static_cast<LocalQUICTokenStatus>(3);
            break;
        case 6:
            reply.status = LocalQUICTokenStatus::Disabled;
            break;
        case 7:
            reply.status = LocalQUICTokenStatus::Refused;
            break;
        }
        CHECK(!encodeLocalQUICTokenReply(reply));
    }
    for (unsigned field = 0; field < 4; ++field) {
        LocalQUICTokenReply refused { .correlation = 19, .daemonInstanceID = 27 };
        switch (field) {
        case 0:
            refused.port = 1;
            break;
        case 1:
            refused.tokenLength = 1;
            break;
        case 2:
            refused.token[0] = 1;
            break;
        case 3:
            refused.certificateSHA256[0] = 1;
            break;
        }
        CHECK(!encodeLocalQUICTokenReply(refused));
    }
}

TEST(localQUICTokenRepliesSurviveEveryBoundaryAndRequireExactCorrelation)
{
    for (const auto& reply : { grant(1), grant(maxQUICTokenLength),
             LocalQUICTokenReply { .correlation = 19, .daemonInstanceID = 27, .status = LocalQUICTokenStatus::Disabled },
             LocalQUICTokenReply { .correlation = 19, .daemonInstanceID = 27, .status = LocalQUICTokenStatus::Refused } }) {
        const auto encoded = encodeLocalQUICTokenReply(reply);
        REQUIRE(encoded);
        for (std::size_t split = 0; split <= encoded->length; ++split) {
            LocalQUICTokenReplyReader reader(19);
            CHECK(reader.consume(encoded->view().first(split))
                == (split == encoded->length ? LocalQUICTokenReplyReader::State::Ready : LocalQUICTokenReplyReader::State::Awaiting));
            CHECK(reader.consume(encoded->view().subspan(split)) == LocalQUICTokenReplyReader::State::Ready);
            const auto decoded = reader.take();
            REQUIRE(decoded);
            CHECK(*decoded == reply);
            CHECK(!reader.take());
        }
        LocalQUICTokenReplyReader bytes(19);
        for (std::size_t index = 0; index < encoded->length; ++index)
            CHECK(bytes.consume(encoded->view().subspan(index, 1))
                == (index + 1 == encoded->length ? LocalQUICTokenReplyReader::State::Ready : LocalQUICTokenReplyReader::State::Awaiting));
        const std::array<std::uint8_t, 1> trailing {};
        CHECK(bytes.consume(trailing) == LocalQUICTokenReplyReader::State::Refused);
        CHECK(!bytes.take());
        LocalQUICTokenReplyReader wrong(20);
        CHECK(wrong.consume(encoded->view()) == LocalQUICTokenReplyReader::State::Refused);
        CHECK(!wrong.take());
    }
    LocalQUICTokenReplyReader zero(0);
    CHECK(zero.consume({}) == LocalQUICTokenReplyReader::State::Refused);
}

TEST(localQUICTokenAuthorityListenerAndReplyAdmissionPrecedeEveryMint)
{
    using namespace Ubiquitty::Host;
    const LocalControlPeer owner { .isSameUserLocalStream = true, .isTrustedOwnerBinary = true };
    const std::string certificate(32, 'c');
    const LocalQUICListener listener { .isEnabled = true, .port = 4321, .certificateSHA256 = certificate };
    unsigned mints = 0;
    const auto mint = [&] { ++mints; return std::string("opaque\0token", 12); };
    for (unsigned missing = 0; missing < 4; ++missing) {
        auto peer = owner;
        switch (missing) {
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
        CHECK(handleLocalQUICToken(19, 27, peer, listener, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Refused);
    }
    CHECK(handleLocalQUICToken(0, 27, owner, listener, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Refused);
    CHECK(handleLocalQUICToken(19, 0, owner, listener, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Refused);
    CHECK(handleLocalQUICToken(19, 27, owner, listener, LocalReplyAdmission::Refused, mint).status == LocalQUICTokenStatus::Refused);
    CHECK(handleLocalQUICToken(19, 27, owner, {}, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Disabled);
    CHECK(handleLocalQUICToken(19, 27, owner, { .isEnabled = true, .port = 0, .certificateSHA256 = certificate }, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Refused);
    CHECK(handleLocalQUICToken(19, 27, owner, { .isEnabled = true, .port = 4321, .certificateSHA256 = "short" }, LocalReplyAdmission::Reserved, mint).status == LocalQUICTokenStatus::Refused);
    CHECK_EQ(mints, 0u);
    const auto granted = handleLocalQUICToken(19, 27, owner, listener, LocalReplyAdmission::Reserved, mint);
    CHECK_EQ(mints, 1u);
    CHECK(granted.status == LocalQUICTokenStatus::Granted);
    CHECK_EQ(granted.tokenLength, 12u);
    CHECK_EQ(granted.token[6], 0u);
    CHECK_EQ(granted.correlation, 19u);
    CHECK_EQ(granted.daemonInstanceID, 27u);
    CHECK(handleLocalQUICToken(19, 27, owner, listener, LocalReplyAdmission::Reserved, [] { return std::string(); }).status == LocalQUICTokenStatus::Refused);
    CHECK(handleLocalQUICToken(19, 27, owner, listener, LocalReplyAdmission::Reserved, [] { return std::string(65, 'x'); }).status == LocalQUICTokenStatus::Refused);
}

TEST(localReplyOutputAdmissionHasExactSubtractiveBoundaries)
{
    using Ubiquitty::Host::fitsClientOutput;
    CHECK(fitsClientOutput(0, 133, 133));
    CHECK(fitsClientOutput(17, 133, 150));
    CHECK(!fitsClientOutput(17, 134, 150));
    CHECK(!fitsClientOutput(150, 0, 150));
    CHECK(!fitsClientOutput(151, 1, 150));
    CHECK(!fitsClientOutput(std::numeric_limits<std::size_t>::max(), 133, 150));
    CHECK(!fitsClientOutput(17, std::numeric_limits<std::size_t>::max(), 150));
}

UBIQUITTY_TEST_MAIN()
