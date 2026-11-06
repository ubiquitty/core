// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_compression.cpp — checkpoint bodies cross deflated, and what a
// deflated transfer may make a client hold (Docs/sync.md §6.5, §7.3, §7.9).
//
// The properties held:
//
// - A checkpoint body above 4 KiB crosses deflated to a client whose Greet
//   accepts deflate, and as it is to one that accepts none.
// - A deflated body is inflated into at most its declared length: one that
//   declares more than the transfer's cap, or inflates past what it declared,
//   is refused before the client holds more (`TransferRefused`); one that does
//   not inflate, or inflates short, is `Malformed`.
// - No checkpoint cap refuses a pane that binds today: a large screen beside a
//   large image still binds and replicates.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Messages.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// Rows of text in a colour per character: a checkpoint body well over 4 KiB
/// that deflates.
///
/// - Parameter rows: How many rows.
/// - Returns: The bytes a program writes.
std::string colouredRows(int rows)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < 70; ++column)
            bytes += "\x1b[38;5;" + std::to_string((row * 70 + column) % 256) + "m" + static_cast<char>('a' + column % 26);
        bytes += "\r\n";
    }
    return bytes;
}

/// The SHA-256 of bytes.
///
/// - Parameter bytes: The bytes.
/// - Returns: The digest.
std::array<std::uint8_t, 32> digestOf(std::span<const std::uint8_t> bytes)
{
    std::array<std::uint8_t, 32> digest {};
    Ubiquitty::Crypto::sha256(bytes, digest);
    return digest;
}

/// Feeds a reassembler one transfer whose payload is `wire`, coded as
/// `compression` with a declared length, in chunks of at most one frame.
///
/// - Parameters:
///   - wire: The payload as it crosses.
///   - maxBytes: The transfer's cap.
///   - compression: Its coding.
///   - inflatedBytes: The length it declares.
/// - Returns: What the last chunk did.
ChunkOutcome reassemble(std::span<const std::uint8_t> wire, std::size_t maxBytes, Compression compression, std::uint32_t inflatedBytes)
{
    BulkReassembler reassembler;
    const bool isExpected = reassembler.expect(1, maxBytes);
    CHECK(isExpected);
    ChunkOutcome outcome = ChunkOutcome::Pending;
    std::uint32_t index = 0;
    for (std::size_t offset = 0; offset < wire.size() || index == 0;) {
        BulkChunk chunk;
        chunk.transfer = 1;
        chunk.index = index++;
        const std::size_t length = std::min(bulkChunkPayloadBytes, wire.size() - offset);
        chunk.bytes = wire.subspan(offset, length);
        offset += length;
        chunk.isLast = offset == wire.size();
        if (chunk.isLast) {
            chunk.sha256 = digestOf(wire);
            chunk.compression = compression;
            chunk.inflatedBytes = inflatedBytes;
        }
        outcome = reassembler.accept(chunk);
        if (outcome != ChunkOutcome::Pending)
            break;
    }
    return outcome;
}

/// Announces a checkpoint transfer to a link's client on its first pane and
/// hands it `wire` in chunks, coded as given.
///
/// - Parameters:
///   - link: The link.
///   - wire: The payload as it crosses.
///   - inflatedBytes: The length it declares.
void sendDeflatedCheckpoint(SyncLink& link, std::span<const std::uint8_t> wire, std::uint32_t inflatedBytes)
{
    constexpr std::uint32_t transfer = 900;
    std::vector<std::uint8_t> head;
    encode(head, CheckpointHead { .generation = 9'999, .transfer = transfer, .frameClass = FrameClass::Structural, .stateDigest = {} });
    std::vector<std::uint8_t> frames;
    REQUIRE(appendFrame(frames, FrameKind::CheckpointHead, link.handle(), head));
    std::uint32_t index = 0;
    for (std::size_t offset = 0; offset < wire.size();) {
        BulkChunk chunk;
        chunk.transfer = transfer;
        chunk.index = index++;
        const std::size_t length = std::min(bulkChunkPayloadBytes, wire.size() - offset);
        chunk.bytes = wire.subspan(offset, length);
        offset += length;
        chunk.isLast = offset == wire.size();
        if (chunk.isLast) {
            chunk.sha256 = digestOf(wire);
            chunk.compression = Compression::Deflate;
            chunk.inflatedBytes = inflatedBytes;
        }
        std::vector<std::uint8_t> body;
        encode(body, chunk);
        REQUIRE(appendFrame(frames, FrameKind::BulkChunk, 0, body));
    }
    link.deliverToClient(frames);
}

} // namespace

// A 200-column checkpoint of coloured rows crosses deflated, in fewer bytes
// than its body, and applies.
TEST(aCheckpointAboveFourKibibytesTravelsDeflated)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.capabilities.add(ClientCapability::DeflateCheckpoints);
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(60, 200), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane(60, 200, colouredRows(50));
    const HostCounters& counters = link.host().counters();
    CHECK(counters.lastCheckpointBodyBytes > minDeflatedCheckpointBytes);
    CHECK_EQ(counters.checkpointsDeflated, std::uint64_t { 1 });
    CHECK_MSG(counters.lastCheckpointWireBytes < counters.lastCheckpointBodyBytes,
        ("a " + std::to_string(counters.lastCheckpointBodyBytes) + "-byte body took " + std::to_string(counters.lastCheckpointWireBytes) + " on the wire").c_str());
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
    std::printf("aCheckpointAboveFourKibibytesTravelsDeflated: a %llu-byte body took %llu bytes deflated\n",
        static_cast<unsigned long long>(counters.lastCheckpointBodyBytes), static_cast<unsigned long long>(counters.lastCheckpointWireBytes));
}

// A client whose Greet accepts no compression — an editor's — is sent the
// body as it is, and it applies.
TEST(aClientThatAcceptsNoCompressionIsSentTheBodyAsItIs)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.capabilities.remove(ClientCapability::DeflateCheckpoints);
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(60, 200), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane(60, 200, colouredRows(50));
    CHECK_EQ(link.host().counters().checkpointsDeflated, std::uint64_t { 0 });
    CHECK_EQ(link.host().counters().lastCheckpointWireBytes, link.host().counters().lastCheckpointBodyBytes);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A bomb: a few kilobytes that inflate to megabytes. Declaring the true length
// past the transfer's cap is refused before anything is allocated; declaring
// less is refused when the inflater fills one byte past it. Either way the
// client never holds more than the declared length and one byte.
TEST(anInflationBombIsRefusedBeforeItsBound)
{
    const std::vector<std::uint8_t> zeros(std::size_t { 9 } << 20, 0);
    const std::vector<std::uint8_t> bomb = Ubiquitty::Codec::Zlib::compress(zeros);
    CHECK(bomb.size() < zeros.size() / 64);
    CHECK(reassemble(bomb, std::size_t { 1 } << 20, Compression::Deflate, static_cast<std::uint32_t>(zeros.size())) == ChunkOutcome::Refused);
    CHECK(reassemble(bomb, std::size_t { 16 } << 20, Compression::Deflate, 4'096) == ChunkOutcome::Refused);
    CHECK(reassemble(bomb, std::size_t { 16 } << 20, Compression::Deflate, static_cast<std::uint32_t>(zeros.size())) == ChunkOutcome::Complete);

    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    sendDeflatedCheckpoint(link, bomb, 4'096);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::TransferRefused);
}

// A stream that is not deflate, or inflates short of what it declared, is
// malformed, not a resource refusal.
TEST(aCorruptOrShortDeflatedBodyIsMalformed)
{
    const std::vector<std::uint8_t> text(10'000, 'x');
    const std::vector<std::uint8_t> deflated = Ubiquitty::Codec::Zlib::compress(text);
    CHECK(reassemble(deflated, 1 << 20, Compression::Deflate, 20'000) == ChunkOutcome::Corrupt);
    std::vector<std::uint8_t> garbage(2'000);
    for (std::size_t index = 0; index < garbage.size(); ++index)
        garbage[index] = static_cast<std::uint8_t>(index * 131 + 7);
    CHECK(reassemble(garbage, 1 << 20, Compression::Deflate, 10'000) == ChunkOutcome::Corrupt);
    CHECK(reassemble(text, 1 << 20, Compression::None, 0) == ChunkOutcome::Complete);

    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    sendDeflatedCheckpoint(link, garbage, 10'000);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);
}

// A last chunk that is not deflated is version 6's layout, compression byte
// and all left out, so a client speaking 6 reads every chunk a host sends it;
// only a client that named DeflateCheckpoints meets the byte.
TEST(anUndeflatedLastChunkIsVersionSixsLayout)
{
    const std::array<std::uint8_t, 3> payload { 1, 2, 3 };
    BulkChunk chunk;
    chunk.transfer = 3;
    chunk.isLast = true;
    chunk.bytes = payload;
    chunk.sha256.fill(0xAB);
    std::vector<std::uint8_t> body;
    encode(body, chunk);
    // transfer, index, last, the bytes' length and bytes, then the digest.
    CHECK_EQ(body.size(), 4u + 4u + 1u + 1u + payload.size() + 32u);
    const std::optional<BulkChunk> decoded = decodeBulkChunk(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->compression == Compression::None);
    // An explicit None byte is not the canonical form.
    body.push_back(0);
    CHECK(!decodeBulkChunk(body).has_value());
}

// A coding the grammar does not know fails the chunk's decode.
TEST(anUnknownCompressionByteIsMalformed)
{
    BulkChunk chunk;
    chunk.transfer = 3;
    chunk.isLast = true;
    chunk.compression = Compression::Deflate;
    chunk.inflatedBytes = 10;
    std::vector<std::uint8_t> body;
    encode(body, chunk);
    REQUIRE(decodeBulkChunk(body).has_value());
    // The coding byte sits before the four-byte inflated length.
    body[body.size() - 5] = 2;
    CHECK(!decodeBulkChunk(body).has_value());

    Greet greet;
    greet.wireVersion = wireVersion;
    greet.capabilities = ClientCapabilities::fromRaw(std::to_underlying(ClientCapability::DeflateCheckpoints));
    std::vector<std::uint8_t> greetBody;
    encode(greetBody, greet);
    const std::optional<Greet> decoded = decodeGreet(greetBody);
    REQUIRE(decoded.has_value());
    CHECK(decoded->capabilities.contains(ClientCapability::DeflateCheckpoints));
    CHECK(!decoded->capabilities.contains(ClientCapability::SpanOps));
    CHECK_EQ(decoded->capabilities.toRaw(), greet.capabilities.toRaw());
    // A capability bit this build does not know is malformed, not ignored: a
    // peer of this fingerprint names only the bits this build names. The
    // varint sits before the Greet's final resume byte. This Greet names only
    // bits below 0x80, so the varint is one byte, and the first unknown bit,
    // the one above every known bit, goes in a second byte.
    REQUIRE(greetBody[greetBody.size() - 2] < 0x80);
    constexpr auto firstUnknownBit = static_cast<unsigned>(std::bit_width(knownClientCapabilityBits));
    static_assert(firstUnknownBit >= 7 && firstUnknownBit < 14, "the unknown bit sits in the varint's second byte");
    std::vector<std::uint8_t> unknownBit = greetBody;
    unknownBit[unknownBit.size() - 2] |= 0x80;
    unknownBit.insert(unknownBit.end() - 1, static_cast<std::uint8_t>(1u << (firstUnknownBit - 7)));
    CHECK_MSG(!decodeGreet(unknownBit).has_value(), "a Greet naming a capability this build does not know was read");
    // The byte is the varint: a known bit set in it reads as that bit.
    std::vector<std::uint8_t> knownBit = greetBody;
    knownBit[knownBit.size() - 2] |= std::to_underlying(ClientCapability::ShowsNoQuestions);
    const std::optional<Greet> known = decodeGreet(knownBit);
    CHECK(known && known->capabilities.contains(ClientCapability::ShowsNoQuestions));
    CHECK(known && known->capabilities.contains(ClientCapability::DeflateCheckpoints));
}

// No checkpoint cap refuses a pane that binds today: a large screen beside a
// large image binds and replicates, deflated or not.
TEST(aPaneWithALargeImageStoreStillBindsAndReplicates)
{
    std::string image;
    const std::vector<std::uint8_t> pixels(std::size_t { 512 } * 512 * 3, 0x5A);
    const std::string encoded = [&] {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t index = 0; index + 2 < pixels.size(); index += 3) {
            const unsigned value = (unsigned { pixels[index] } << 16) | (unsigned { pixels[index + 1] } << 8) | pixels[index + 2];
            out += alphabet[(value >> 18) & 63];
            out += alphabet[(value >> 12) & 63];
            out += alphabet[(value >> 6) & 63];
            out += alphabet[value & 63];
        }
        return out;
    }();
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4'096) {
        const bool isFirst = offset == 0;
        const bool isMore = offset + 4'096 < encoded.size();
        image += isFirst ? "\x1b_Ga=T,f=24,s=512,v=512,i=5,q=2,m=" : "\x1b_Gm=";
        image += isMore ? "1;" : "0;";
        image += encoded.substr(offset, 4'096);
        image += "\x1b\\";
    }
    for (const bool isDeflated : { true, false }) { // bool-ok: the two runs of one case
        ClientPolicy policy = UbiquittyTest::syncClientPolicy;
        policy.capabilities.set(ClientCapability::DeflateCheckpoints, isDeflated);
        SyncLink link(0, UbiquittyTest::syncDefaultFacts(200, 400), std::nullopt, ClientKind::Human, std::nullopt, policy);
        (void)link.addPane(200, 400, image + colouredRows(190));
        CHECK(link.client().isPaneCurrent(link.handle()));
        // The image's rows scroll the screen, and the lines it pushed into
        // history travel on the history plane after the checkpoint
        // (Docs/sync.md §6.6), so the client is compared once they land.
        link.stepUntilQuiet();
        CHECK_STR_EQ(link.disagreement(link.handle()), "");
    }
}

UBIQUITTY_TEST_MAIN()
