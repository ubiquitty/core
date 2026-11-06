// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/QUICBootstrapReader.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Harness.h"
#include "Platform/Host/LocalControlClient.h"
#include "StreamPartitions.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct OwnedFrame {
    FrameKind kind;
    std::uint32_t channel;
    Bytes body;

    friend bool operator==(const OwnedFrame&, const OwnedFrame&) = default;
};

struct Stream {
    Bytes bytes;
    std::vector<OwnedFrame> frames;
};

Stream makeStream(std::span<const std::size_t> lengths)
{
    Stream stream;
    for (const auto length : lengths) {
        const auto index = stream.frames.size();
        OwnedFrame frame { .kind = index % 2 ? FrameKind::Update : FrameKind::Probe,
            .channel = index % 2 ? (index % 4 == 1 ? 0x1FFFFFFFu : 128u) : 0u,
            .body = Bytes(length) };
        for (std::size_t byte = 0; byte < length; ++byte)
            frame.body[byte] = static_cast<std::uint8_t>((byte * 37 + byte / 251 + index) % 256);
        CHECK(appendFrame(stream.bytes, frame.kind, frame.channel, frame.body));
        stream.frames.push_back(std::move(frame));
    }
    return stream;
}

bool checkFrames(const Stream& stream, std::span<const std::size_t> ends)
{
    FrameDecoder decoder;
    FrameIngress ingress(maxFrameBodyBytes);
    std::vector<OwnedFrame> decoded;
    std::vector<OwnedFrame> delivered;
    bool isValid = true;
    feedStreamPartition(std::span<const std::uint8_t>(stream.bytes), ends, [&](auto piece) {
        decoder.append(piece);
        while (const auto frame = decoder.next())
            decoded.push_back({ .kind = frame->kind, .channel = frame->channel, .body = Bytes(frame->body.begin(), frame->body.end()) });
        const auto result = ingress.consume(piece, [](auto headerBytes) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            const auto state = readFrameHeader(headerBytes, header, error);
            return FrameHeaderDecision { .state = state == FrameHeaderState::Complete ? FrameHeaderDecision::State::Admitted
                    : state == FrameHeaderState::Failed ? FrameHeaderDecision::State::Refused : FrameHeaderDecision::State::NeedsMore,
                .bodyBytes = header.bodyBytes }; }, [&](auto headerBytes, auto body) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            isValid &= readFrameHeader(headerBytes, header, error) == FrameHeaderState::Complete;
            delivered.push_back({ .kind = header.kind, .channel = header.channel, .body = Bytes(body.begin(), body.end()) });
            return FrameDelivery::Continue; });
        isValid &= result.consumedBytes == piece.size() && result.state == FrameIngress::State::Accepting;
        isValid &= ingress.retainedBytes() <= maxFrameBodyBytes;
    });
    return isValid && decoder.error() == EnvelopeError::None && !decoder.bufferedBytes()
        && !ingress.pendingBytes() && decoded == stream.frames && delivered == stream.frames;
}

} // namespace

TEST(syncFramesKeepEveryByteAcrossAllSplitsAndSeededPartitions)
{
    const std::array<std::size_t, 7> lengths { 0, 1, 127, 128, 255, 256, 3 };
    const auto stream = makeStream(lengths);
    CHECK(sweepStreamPartitions(stream.bytes.size(), PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
        const bool matches = checkFrames(stream, ends);
        CHECK_MSG(matches, label.c_str());
        return matches;
    }));
}

TEST(syncFramesAtVarintAndBodyBoundsKeepTheirContentsAndNextFrame)
{
    for (const std::size_t bound : std::array<std::size_t, 2> { 16384, maxFrameBodyBytes }) {
        const std::array<std::size_t, 5> lengths { bound - 1, bound, 0, 1, 17 };
        const auto stream = makeStream(lengths);
        CHECK(sweepStreamPartitions(stream.bytes.size(), PartitionCoverage::Boundaries, [&](auto ends, const std::string& label) {
            const bool matches = checkFrames(stream, ends);
            CHECK_MSG(matches, ("bound=" + std::to_string(bound) + " " + label).c_str());
            return matches;
        }));
    }
}

TEST(quicPreamblesPreserveTheFirstCoalescedSyncFramesAcrossPartitions)
{
    ResumeProof proof { .attachmentID = { 17, 29 }, .mac = {} };
    for (std::size_t index = 0; index < proof.mac.size(); ++index)
        proof.mac[index] = static_cast<std::uint8_t>(index * 7);
    std::string token;
    for (std::size_t index = 0; index < maxPreambleTokenBytes; ++index)
        token.push_back(static_cast<char>('a' + index % 26));
    const std::array preambles { Preamble(TokenPreamble { .token = token }), Preamble(proof) };
    const std::array<std::size_t, 3> lengths { 1, 129, 0 };
    const auto stream = makeStream(lengths);
    for (const auto& expected : preambles) {
        Bytes bytes;
        encodePreamble(bytes, expected);
        bytes.insert(bytes.end(), stream.bytes.begin(), stream.bytes.end());
        CHECK(sweepStreamPartitions(bytes.size(), PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
            PreambleReader reader;
            std::optional<Preamble> preamble;
            Bytes frames;
            bool isBounded = true;
            feedStreamPartition(std::span<const std::uint8_t>(bytes), ends, [&](auto piece) {
                const auto consumed = preamble ? 0 : reader.feed(piece);
                isBounded &= reader.heldBytes() <= maxPreambleBytes;
                if (!preamble && reader.status() == PreambleReader::Status::Complete)
                    preamble = reader.take();
                frames.insert(frames.end(), piece.begin() + consumed, piece.end());
            });
            const bool matches = isBounded && preamble == expected && frames == stream.bytes;
            CHECK_MSG(matches, label.c_str());
            return matches;
        }));
    }
}

TEST(localControlRequestsAndRepliesPreserveTheirIdentityAcrossPartitions)
{
    namespace Local = Ubiquitty::Protocol::Local;
    namespace Host = Ubiquitty::Host;
    constexpr std::uint64_t correlation = 0x0807060504030201;
    const auto request = Local::encodeLocalReloadRequest(correlation);
    CHECK(sweepStreamPartitions(request.size(), PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
        Local::LocalControlReader reader;
        Host::LocalReloadExchange exchange(correlation, 100);
        exchange.authorize({ .hasPrivateOwnerDirectory = true, .hasPrivateOwnerSocket = true, .hasSameUserPeer = true }, 1);
        Bytes written;
        feedStreamPartition(std::span<const std::uint8_t>(request), ends, [&](auto piece) {
            (void)reader.consume(piece);
            const auto outgoing = exchange.outgoing(1).first(piece.size());
            written.insert(written.end(), outgoing.begin(), outgoing.end());
            if (!piece.empty())
                exchange.didSend(piece.size(), 1);
        });
        const auto decoded = reader.take();
        const bool matches = decoded && decoded->correlation == correlation && decoded->operation == Local::LocalControlOperation::ReloadCredentials
            && std::ranges::equal(written, request) && exchange.state(1) == Host::LocalReloadExchange::State::AwaitingReply;
        CHECK_MSG(matches, label.c_str());
        return matches;
    }));
    const Local::LocalControlReply reply { .correlation = correlation, .daemonInstanceID = 0x1817161514131211, .status = Local::LocalControlStatus::Reloaded };
    const auto bytes = Local::encodeLocalControlReply(reply);
    CHECK(sweepStreamPartitions(bytes.size(), PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
        Host::LocalReloadExchange exchange(correlation, 100);
        exchange.authorize({ .hasPrivateOwnerDirectory = true, .hasPrivateOwnerSocket = true, .hasSameUserPeer = true }, 1);
        exchange.didSend(request.size(), 1);
        feedStreamPartition(std::span<const std::uint8_t>(bytes), ends, [&](auto piece) { exchange.receive(piece, 2); });
        const bool matches = exchange.state(2) == Host::LocalReloadExchange::State::Reloaded;
        CHECK_MSG(matches, label.c_str());
        return matches;
    }));
}

TEST(localQUICTokenRepliesAtTheirBoundPreserveAllBytesAcrossPartitions)
{
    namespace Local = Ubiquitty::Protocol::Local;
    Local::LocalQUICTokenReply reply { .correlation = 19, .daemonInstanceID = 27 };
    reply.status = Local::LocalQUICTokenStatus::Granted;
    reply.port = 32123;
    reply.tokenLength = Local::maxQUICTokenLength;
    for (std::size_t index = 0; index < reply.token.size(); ++index)
        reply.token[index] = static_cast<std::uint8_t>('a' + index % 26);
    reply.certificateSHA256.fill('b');
    const auto encoded = Local::encodeLocalQUICTokenReply(reply);
    REQUIRE(encoded);
    const auto bytes = encoded->view();
    CHECK(sweepStreamPartitions(bytes.size(), PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
        Local::LocalQUICTokenReplyReader reader(reply.correlation);
        feedStreamPartition(bytes, ends, [&](auto piece) { (void)reader.consume(piece); });
        const bool matches = reader.take() == reply;
        CHECK_MSG(matches, label.c_str());
        return matches;
    }));
}

TEST(quicBootstrapLinesAtTheirBoundHaveTheSameDecisionAcrossPartitions)
{
    const std::string line = "quic_port=32123 quic_token=0123456789abcdef quic_cert_sha256=" + std::string(64, 'b') + "\r\n";
    for (const std::size_t bannerBytes : std::array<std::size_t, 4> { 0, maxQUICBootstrapBufferSize - 1, maxQUICBootstrapBufferSize, maxQUICBootstrapBufferSize + 1 }) {
        const std::string text = std::string(bannerBytes, 'x') + "\n" + line;
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        QUICBootstrapReader whole;
        const auto expected = whole.feed(bytes);
        CHECK(expected == (bannerBytes > maxQUICBootstrapBufferSize ? QUICBootstrapReader::State::Refused : QUICBootstrapReader::State::Ready));
        CHECK(sweepStreamPartitions(bytes.size(), bannerBytes == 0 ? PartitionCoverage::Exhaustive : PartitionCoverage::Boundaries, [&](auto ends, const std::string& label) {
            QUICBootstrapReader reader;
            feedStreamPartition(bytes, ends, [&](auto piece) { (void)reader.feed(piece); });
            const bool matches = reader.state() == expected && reader.refusalReason() == whole.refusalReason()
                && (expected != QUICBootstrapReader::State::Ready || (reader.info().port == whole.info().port && reader.info().token == whole.info().token && reader.info().certificateSHA256 == whole.info().certificateSHA256));
            CHECK_MSG(matches, ("banner=" + std::to_string(bannerBytes) + " " + label).c_str());
            return matches;
        }));
    }
}

UBIQUITTY_TEST_MAIN()
