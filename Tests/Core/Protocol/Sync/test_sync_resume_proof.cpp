// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_resume_proof — resume without SSH (Docs/sync.md §6.9, decision
// D16): HMAC-SHA256 against RFC 4231; a proof bound to one connection's
// exporter continues an attachment whose Greet then carries no secret; and
// the proof is refused on another connection, for a detached attachment, for
// another host instance, a second time, and while a resume is in progress. A
// connection a proof admitted resumes that attachment and does nothing else.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Crypto/HMAC.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ResumeProof.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;
using Ubiquitty::Crypto::hmacSHA256;

namespace {

std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

std::array<std::uint8_t, 32> macOf(std::string_view hex)
{
    std::array<std::uint8_t, 32> mac {};
    const auto nibble = [](char digit) { return static_cast<std::uint8_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10); };
    for (std::size_t index = 0; index < 32 && 2 * index + 1 < hex.size(); ++index)
        mac[index] = static_cast<std::uint8_t>(nibble(hex[2 * index]) << 4 | nibble(hex[2 * index + 1]));
    return mac;
}

/// An exporter a test names by its first byte.
std::array<std::uint8_t, 32> exporterOf(std::uint8_t first)
{
    std::array<std::uint8_t, 32> exporter {};
    for (std::size_t index = 0; index < exporter.size(); ++index)
        exporter[index] = static_cast<std::uint8_t>(first + index);
    return exporter;
}

/// A client attached, one key written, and its transport gone: the state a
/// redial with a proof starts from.
struct Dropped {
    Host host { { 41, 42 } };
    ClientAttachment client { UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity() };
    std::uint64_t now = 1;
    ID128 attachmentID;

    Dropped()
    {
        attachFresh(host, client, now);
        CHECK(client.send(paneID, raw("x"), now).has_value());
        pumpFor(host, client, now, 50'000);
        attachmentID = client.held()->attachmentID;
        drop(host, client, now);
    }

    /// The host accepts a connection and is shown a proof for it, as its
    /// transport does before any frame.
    std::expected<const AttachmentRecord*, ResumeRefusal> acceptWith(const ResumeProof& proof, std::span<const std::uint8_t, 32> exporter)
    {
        host.accept();
        return host.table.admitProof(proof, exporter, host.connectionNumber, now);
    }
};

/// The secret a Greet in a client's first bytes carries.
std::optional<std::optional<ID128>> greetSecretIn(std::span<const std::uint8_t> stream)
{
    FrameDecoder decoder;
    decoder.append(stream);
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind != FrameKind::Greet)
            continue;
        const std::optional<Greet> greet = decodeGreet(frame->body);
        if (!greet || !greet->resume)
            return std::nullopt;
        return greet->resume->resumeSecret;
    }
    return std::nullopt;
}

} // namespace

// RFC 4231's test cases: a short key, a key shorter than the MAC, a key and
// data of repeated bytes, and keys longer than a block, which are hashed
// first.
TEST(hmacSHA256MatchesRFC4231)
{
    CHECK(hmacSHA256(bytesOf("Jefe"), bytesOf("what do ya want for nothing?")) == macOf("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK(hmacSHA256(std::vector<std::uint8_t>(20, 0x0b), bytesOf("Hi There")) == macOf("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK(hmacSHA256(std::vector<std::uint8_t>(20, 0xaa), std::vector<std::uint8_t>(50, 0xdd)) == macOf("773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));
    CHECK(hmacSHA256(std::vector<std::uint8_t>(131, 0xaa), bytesOf("Test Using Larger Than Block-Size Key - Hash Key First")) == macOf("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    CHECK(hmacSHA256(std::vector<std::uint8_t>(131, 0xaa), bytesOf("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.")) == macOf("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
}

// Two MACs compare equal only when every byte is.
TEST(aMACComparesEqualOnlyToItself)
{
    const std::array<std::uint8_t, 32> mac = macOf("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(Ubiquitty::Crypto::constantTimeEqual(mac, mac));
    for (const std::size_t index : { std::size_t { 0 }, std::size_t { 15 }, std::size_t { 31 } }) {
        std::array<std::uint8_t, 32> other = mac;
        other[index] ^= 0x01;
        CHECK(!Ubiquitty::Crypto::constantTimeEqual(mac, other));
    }
}

// The MAC is HMAC-SHA256 keyed with the secret's wire bytes over the
// exporter, the attachment and the host instance, in that order, and moves
// with each of them.
TEST(theProofMACIsBoundToTheExporterTheAttachmentAndTheInstance)
{
    constexpr ID128 secret { .low = 0x1111111111111111ull, .high = 0x2222222222222222ull };
    constexpr ID128 attachment { .low = 0x0102030405060708ull, .high = 0x1112131415161718ull };
    constexpr ID128 instance { .low = 41, .high = 42 };
    const std::array<std::uint8_t, 32> exporter = exporterOf(7);
    const std::array<std::uint8_t, 32> mac = resumeProofMAC(secret, exporter, attachment, instance);

    std::vector<std::uint8_t> key(8, 0x11);
    key.insert(key.end(), 8, 0x22);
    std::vector<std::uint8_t> message(exporter.begin(), exporter.end());
    for (const std::uint8_t byte : { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11 })
        message.push_back(byte);
    message.push_back(41);
    message.insert(message.end(), 7, 0);
    message.push_back(42);
    message.insert(message.end(), 7, 0);
    CHECK(mac == hmacSHA256(key, message));

    constexpr ID128 otherSecret { .low = 0x1111111111111112ull, .high = 0x2222222222222222ull };
    constexpr ID128 otherAttachment { .low = 0x0102030405060709ull, .high = 0x1112131415161718ull };
    constexpr ID128 otherInstance { .low = 41, .high = 43 };
    CHECK(resumeProofMAC(otherSecret, exporter, attachment, instance) != mac);
    CHECK(resumeProofMAC(secret, exporterOf(8), attachment, instance) != mac);
    CHECK(resumeProofMAC(secret, exporter, otherAttachment, instance) != mac);
    CHECK(resumeProofMAC(secret, exporter, attachment, otherInstance) != mac);
}

// A proof travels as the attachment's identity and the MAC, 48 bytes, and
// nothing of another length reads as one.
TEST(theResumePreambleRoundTrips)
{
    const ResumeProof proof { .attachmentID = { .low = 0x0102030405060708ull, .high = 0x1112131415161718ull }, .mac = macOf("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") };
    std::vector<std::uint8_t> bytes;
    encodeResumePreamble(bytes, proof);
    REQUIRE(bytes.size() == resumeProofBytes);
    CHECK_EQ(bytes[0], 0x08);
    CHECK_EQ(bytes[15], 0x11);
    CHECK_EQ(bytes[16], 0x5b);
    CHECK_EQ(bytes[47], 0x43);
    CHECK(decodeResumePreamble(bytes) == std::optional { proof });
    CHECK(!decodeResumePreamble(std::span { bytes }.first(resumeProofBytes - 1)).has_value());
    bytes.push_back(0);
    CHECK(!decodeResumePreamble(bytes).has_value());
    CHECK(!decodeResumePreamble({}).has_value());
}

// A client whose transport died redials and proves it holds the attachment.
// The Greet on that connection carries no secret, and the attachment
// continues: no checkpoint, and its input in order, once.
TEST(aProofContinuesTheAttachmentWithoutASecretInGreet)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    CHECK(proof->attachmentID == dropped.attachmentID);
    const std::expected<const AttachmentRecord*, ResumeRefusal> admitted = dropped.acceptWith(*proof, exporter);
    REQUIRE(admitted.has_value());
    CHECK((*admitted)->attachmentID == dropped.attachmentID);
    CHECK((*admitted)->authority.principal == owner.principal);
    CHECK(dropped.host.table.provenAttachment(dropped.host.connectionNumber) == std::optional { dropped.attachmentID });
    const ID128 secretBefore = dropped.client.held()->resumeSecret;

    (void)dropped.client.connect(dropped.now, ResumeCarrier::Proof);
    const std::vector<std::uint8_t> greeting = dropped.client.takeStreamOutput();
    const std::optional<std::optional<ID128>> sent = greetSecretIn(greeting);
    REQUIRE(sent.has_value());
    CHECK(!sent->has_value());
    dropped.host.connection->receiveStream(greeting, dropped.now);
    pumpFor(dropped.host, dropped.client, dropped.now, 200'000);
    REQUIRE(dropped.client.state() == ClientAttachmentState::Live);
    CHECK(dropped.client.held()->attachmentID == dropped.attachmentID);
    CHECK(!dropped.host.connection->resumeRefusal().has_value());
    CHECK_EQ(dropped.host.connection->counters().checkpointsSent, 0u);
    // The Welcome issued a fresh secret, and the proof's wait is over.
    CHECK(dropped.client.held()->resumeSecret != secretBefore);
    CHECK(!dropped.host.table.provenAttachment(dropped.host.connectionNumber).has_value());

    REQUIRE(dropped.client.send(paneID, raw("y"), dropped.now).has_value());
    pumpFor(dropped.host, dropped.client, dropped.now, 50'000);
    CHECK(std::string(dropped.host.pane.writer.bytes().begin(), dropped.host.pane.writer.bytes().end()) == "xy");
}

// A proof read off one connection is worth nothing on another: its MAC is
// over that connection's exporter.
TEST(aProofReplayedUnderAnotherExporterIsRefused)
{
    Dropped dropped;
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporterOf(1));
    REQUIRE(proof.has_value());
    const std::expected<const AttachmentRecord*, ResumeRefusal> replayed = dropped.acceptWith(*proof, exporterOf(2));
    REQUIRE(!replayed.has_value());
    CHECK(replayed.error() == ResumeRefusal::WrongSecret);
    CHECK(!dropped.host.table.provenAttachment(dropped.host.connectionNumber).has_value());
    // The attachment is untouched: its own client still continues it.
    CHECK(dropped.acceptWith(*proof, exporterOf(1)).has_value());
}

// A client that left for good has no attachment to continue: a detached one
// goes back through SSH.
TEST(aProofForADetachedAttachmentIsRefused)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    dropped.host.table.noteDetached(dropped.attachmentID);
    const std::expected<const AttachmentRecord*, ResumeRefusal> refused = dropped.acceptWith(*proof, exporter);
    REQUIRE(!refused.has_value());
    CHECK(refused.error() == ResumeRefusal::UnknownAttachment);
}

// The MAC names the host instance the client held. One made for another
// instance, or an identity another instance issued, is refused.
TEST(aProofNamingAnUnknownHostInstanceIsRefused)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const ID128 secret = dropped.client.held()->resumeSecret;
    constexpr ID128 elsewhere { .low = 99, .high = 99 };
    const ResumeProof otherInstance { .attachmentID = dropped.attachmentID, .mac = resumeProofMAC(secret, exporter, dropped.attachmentID, elsewhere) };
    const std::expected<const AttachmentRecord*, ResumeRefusal> refused = dropped.acceptWith(otherInstance, exporter);
    REQUIRE(!refused.has_value());
    CHECK(refused.error() == ResumeRefusal::WrongSecret);

    const ID128 foreign { .low = dropped.attachmentID.low, .high = dropped.attachmentID.high + 1 };
    const ResumeProof otherIdentity { .attachmentID = foreign, .mac = resumeProofMAC(secret, exporter, foreign, { 41, 42 }) };
    const std::expected<const AttachmentRecord*, ResumeRefusal> unknown = dropped.acceptWith(otherIdentity, exporter);
    REQUIRE(!unknown.has_value());
    CHECK(unknown.error() == ResumeRefusal::UnknownAttachment);
}

// A proof is good once: the resume it admitted consumed the secret it was
// made with, so the same bytes are refused afterwards, exporter and all.
TEST(aProofIsSingleUse)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    (void)dropped.client.connect(dropped.now, ResumeCarrier::Proof);
    pumpFor(dropped.host, dropped.client, dropped.now, 200'000);
    REQUIRE(dropped.client.state() == ClientAttachmentState::Live);

    drop(dropped.host, dropped.client, dropped.now);
    const std::expected<const AttachmentRecord*, ResumeRefusal> again = dropped.acceptWith(*proof, exporter);
    REQUIRE(!again.has_value());
    CHECK(again.error() == ResumeRefusal::WrongSecret);
}

// A second proof for the attachment, on another connection, takes the place
// of one whose connection has not greeted: the client redialled, and the
// host may not see the first dial die for a long time. The first connection
// is listed to be closed and may no longer resume.
TEST(aNewerProofReplacesOneThatHasNotGreeted)
{
    Dropped dropped;
    const std::optional<ResumeProof> first = dropped.client.resumeProof(exporterOf(1));
    const std::optional<ResumeProof> second = dropped.client.resumeProof(exporterOf(2));
    REQUIRE(first.has_value() && second.has_value());
    REQUIRE(dropped.acceptWith(*first, exporterOf(1)).has_value());
    const std::uint64_t firstConnection = dropped.host.connectionNumber;
    CHECK(dropped.host.table.takeSuperseded().empty());
    // The same proof shown again on its own connection changes nothing.
    CHECK(dropped.host.table.admitProof(*first, exporterOf(1), firstConnection, dropped.now).has_value());
    CHECK(dropped.host.table.takeSuperseded().empty());

    REQUIRE(dropped.acceptWith(*second, exporterOf(2)).has_value());
    const std::uint64_t secondConnection = dropped.host.connectionNumber;
    CHECK(dropped.host.table.takeSuperseded() == std::vector<std::uint64_t> { firstConnection });
    CHECK(!dropped.host.table.provenAttachment(firstConnection).has_value());
    CHECK(dropped.host.table.provenAttachment(secondConnection) == std::optional { dropped.attachmentID });
    // The first connection going changes nothing for the second.
    dropped.host.table.abandonProof(firstConnection);
    CHECK(dropped.host.table.provenAttachment(secondConnection) == std::optional { dropped.attachmentID });
    dropped.host.table.abandonProof(secondConnection);
    CHECK(!dropped.host.table.provenAttachment(secondConnection).has_value());
}

// The secret is never sent where a proof stood in for it: a Greet that
// carries one on a proof's connection is malformed, and the attachment is
// left for its client to continue.
TEST(aGreetWithASecretOnAProofConnectionCloses)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    const std::uint64_t connection = dropped.host.connectionNumber;
    (void)dropped.client.connect(dropped.now, ResumeCarrier::Secret);
    pump(dropped.host, dropped.client, dropped.now);
    pump(dropped.host, dropped.client, dropped.now);
    CHECK(dropped.host.connection->state() == ConnectionState::Closed);
    CHECK(dropped.host.connection->byeReason() == std::optional { ByeReason::Malformed });
    CHECK(!dropped.host.table.provenAttachment(connection).has_value());
    REQUIRE(dropped.host.table.find(dropped.attachmentID) != nullptr);
    CHECK(dropped.host.table.find(dropped.attachmentID)->state == AttachmentState::Lost);
    // At the table itself, too: a secret is not taken where a proof stood.
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    const ResumeRequest withSecret { .attachmentID = dropped.attachmentID, .resumeSecret = dropped.client.held()->resumeSecret, .panes = {} };
    const std::expected<AttachmentRecord*, ResumeRefusal> refused = dropped.host.table.resume(withSecret, owner, dropped.host.connectionNumber, dropped.now);
    REQUIRE(!refused.has_value());
    CHECK(refused.error() == ResumeRefusal::WrongSecret);
}

// A proof authorises one thing. A Greet on its connection that attaches
// fresh, or resumes another attachment, creates nothing and is closed.
TEST(aProofConnectionAttachesNothingFresh)
{
    Dropped dropped;
    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    ClientIdentity stranger = clientIdentity();
    stranger.clientInstance = { 70, 80 };
    ClientAttachment fresh(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), stranger);
    (void)fresh.connect(dropped.now);
    pump(dropped.host, fresh, dropped.now);
    pump(dropped.host, fresh, dropped.now);
    CHECK(dropped.host.connection->state() == ConnectionState::Closed);
    CHECK(dropped.host.connection->byeReason() == std::optional { ByeReason::Malformed });
    constexpr ID128 strangerInstance { 70, 80 };
    CHECK(!dropped.host.table.holdsClient(strangerInstance));
    CHECK(!fresh.held().has_value());

    // Naming another attachment than the one proved is the same.
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    ClientAttachment other(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), stranger);
    other.restore({ .hostInstanceID = { 41, 42 }, .attachmentID = { .low = dropped.attachmentID.low + 7, .high = dropped.attachmentID.high }, .resumeSecret = { 1, 2 }, .panes = {} });
    (void)other.connect(dropped.now, ResumeCarrier::Proof);
    pump(dropped.host, other, dropped.now);
    pump(dropped.host, other, dropped.now);
    CHECK(dropped.host.connection->byeReason() == std::optional { ByeReason::Malformed });
    CHECK(!dropped.host.table.holdsClient(strangerInstance));
}

// A proof for one attachment does not carry another: a Greet that resumes a
// second attachment on the proof's connection, with that attachment's own
// right secret, is closed, and the second attachment stays where it was.
TEST(aProofConnectionResumesNoOtherAttachment)
{
    Dropped dropped;
    ClientIdentity secondIdentity = clientIdentity();
    secondIdentity.clientInstance = { 70, 80 };
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), secondIdentity);
    attachFresh(dropped.host, second, dropped.now);
    REQUIRE(second.held().has_value());
    const ID128 secondID = second.held()->attachmentID;
    REQUIRE(secondID != dropped.attachmentID);
    drop(dropped.host, second, dropped.now);

    const std::array<std::uint8_t, 32> exporter = exporterOf(1);
    const std::optional<ResumeProof> proof = dropped.client.resumeProof(exporter);
    REQUIRE(proof.has_value());
    REQUIRE(dropped.acceptWith(*proof, exporter).has_value());
    (void)second.connect(dropped.now, ResumeCarrier::Secret);
    pump(dropped.host, second, dropped.now);
    pump(dropped.host, second, dropped.now);
    CHECK(dropped.host.connection->state() == ConnectionState::Closed);
    CHECK(dropped.host.connection->byeReason() == std::optional { ByeReason::Malformed });
    REQUIRE(dropped.host.table.find(secondID) != nullptr);
    CHECK(dropped.host.table.find(secondID)->state == AttachmentState::Lost);
}

// Without a proof, a resume that carries no secret is refused like a wrong
// one: leaving the secret out proves nothing. The connection attaches fresh,
// and the attachment its client left Lost is freed: the same principal's
// same client is back, and holds one attachment.
TEST(aSecretlessResumeWithoutAProofIsRefused)
{
    Dropped dropped;
    dropped.host.accept();
    (void)dropped.client.connect(dropped.now, ResumeCarrier::Proof);
    pump(dropped.host, dropped.client, dropped.now);
    pump(dropped.host, dropped.client, dropped.now);
    CHECK(dropped.host.connection->resumeRefusal() == std::optional { ResumeRefusal::WrongSecret });
    REQUIRE(dropped.host.connection->attachment() != nullptr);
    CHECK(dropped.host.connection->attachment()->attachmentID != dropped.attachmentID);
    CHECK(dropped.host.table.find(dropped.attachmentID) == nullptr);
}

// A client that holds no attachment has nothing to prove: a fresh attach
// goes through SSH.
TEST(aClientThatHoldsNothingHasNoProof)
{
    const ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    CHECK(!client.resumeProof(exporterOf(1)).has_value());
}

// A preamble leads with its kind: a token under its length, or a proof. Each
// reads back whole however the stream cuts it, and the reader takes none of
// the frames that follow.
TEST(aPreambleReadsBackHoweverItIsCut)
{
    const Preamble token = TokenPreamble { .token = "one-time-token" };
    ResumeProof proof { .attachmentID = { 7, 8 }, .mac = {} };
    for (std::size_t index = 0; index < proof.mac.size(); ++index)
        proof.mac[index] = static_cast<std::uint8_t>(index * 3 + 1);
    for (const Preamble& preamble : { token, Preamble { proof } }) {
        std::vector<std::uint8_t> bytes;
        encodePreamble(bytes, preamble);
        CHECK(bytes.size() <= maxPreambleBytes);
        const std::size_t preambleBytes = bytes.size();
        const std::array<std::uint8_t, 3> frames { 0xAA, 0xBB, 0xCC };
        bytes.insert(bytes.end(), frames.begin(), frames.end());
        for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
            PreambleReader reader;
            const std::size_t first = reader.feed(std::span { bytes }.first(cut));
            CHECK_EQ(first, std::min(cut, preambleBytes));
            CHECK(reader.status() == (cut >= preambleBytes ? PreambleReader::Status::Complete : PreambleReader::Status::Incomplete));
            if (cut < preambleBytes) {
                PreambleReader partial;
                (void)partial.feed(std::span { bytes }.first(cut));
                CHECK(!partial.take().has_value());
            }
            const std::size_t second = reader.feed(std::span { bytes }.subspan(first));
            CHECK_EQ(first + second, preambleBytes);
            REQUIRE(reader.status() == PreambleReader::Status::Complete);
            CHECK_EQ(reader.feed(frames), 0u);
            CHECK(reader.take() == std::optional { preamble });
            CHECK_EQ(reader.heldBytes(), 0u);
        }
    }
    // The carrier writes the token's kind as a number of its own.
    CHECK_EQ(std::to_underlying(PreambleKind::Token), 1);
    CHECK_EQ(std::to_underlying(PreambleKind::Resume), 2);
    CHECK_EQ(maxPreambleBytes, 1u + 1u + maxPreambleTokenBytes);
}

// The preamble a resuming client sends is the resume kind and the proof the
// host computes for the same connection, and no other connection's.
TEST(theResumePreambleIsTheProofForItsConnection)
{
    constexpr ID128 secret { 5, 6 };
    constexpr ID128 attachmentID { 7, 8 };
    constexpr ID128 instance { 1, 2 };
    const std::vector<std::uint8_t> bytes = resumePreamble(secret, exporterOf(1), attachmentID, instance);
    PreambleReader reader;
    CHECK_EQ(reader.feed(bytes), bytes.size());
    const std::optional<Preamble> read = reader.take();
    REQUIRE(read && std::holds_alternative<ResumeProof>(*read));
    const ResumeProof& proof = std::get<ResumeProof>(*read);
    CHECK(proof.attachmentID == attachmentID);
    CHECK(proof.mac == resumeProofMAC(secret, exporterOf(1), attachmentID, instance));
    CHECK(proof.mac != resumeProofMAC(secret, exporterOf(2), attachmentID, instance));
}

// What is no preamble is refused at the byte that says so, and nothing of it
// is held: an unknown kind, a token of no length, a token too long.
TEST(aMalformedPreambleIsRefusedAndHoldsNothing)
{
    const std::vector<std::vector<std::uint8_t>> malformed {
        { 0 },
        { 3 },
        { 0xFF, 1, 2, 3 },
        { std::to_underlying(PreambleKind::Token), 0 },
        { std::to_underlying(PreambleKind::Token), static_cast<std::uint8_t>(maxPreambleTokenBytes + 1), 'x' },
    };
    for (const std::vector<std::uint8_t>& bytes : malformed) {
        PreambleReader reader;
        (void)reader.feed(bytes);
        CHECK(reader.status() == PreambleReader::Status::Malformed);
        CHECK_EQ(reader.heldBytes(), 0u);
        CHECK(!reader.take().has_value());
        CHECK_EQ(reader.feed(bytes), 0u);
    }
    // The longest token is taken.
    std::vector<std::uint8_t> longest { std::to_underlying(PreambleKind::Token), static_cast<std::uint8_t>(maxPreambleTokenBytes) };
    longest.resize(2 + maxPreambleTokenBytes, 't');
    PreambleReader reader;
    CHECK_EQ(reader.feed(longest), longest.size());
    CHECK(reader.status() == PreambleReader::Status::Complete);
}

// However much an unauthenticated peer sends, the reader holds no more than a
// preamble: a megabyte under a proof's kind is taken a proof's worth.
TEST(aReaderHoldsNoMoreThanAPreamble)
{
    std::vector<std::uint8_t> flood(std::size_t { 1 } << 20, std::to_underlying(PreambleKind::Resume));
    PreambleReader reader;
    std::size_t taken = 0;
    for (std::size_t offset = 0; offset < flood.size(); offset += 4'096) {
        taken += reader.feed(std::span { flood }.subspan(offset, 4'096));
        CHECK(reader.heldBytes() <= maxPreambleBytes);
    }
    CHECK_EQ(taken, 1u + resumeProofBytes);
    std::vector<std::uint8_t> tokens(std::size_t { 1 } << 16, std::to_underlying(PreambleKind::Token));
    tokens[1] = static_cast<std::uint8_t>(maxPreambleTokenBytes);
    PreambleReader tokenReader;
    CHECK_EQ(tokenReader.feed(tokens), 2u + maxPreambleTokenBytes);
    CHECK(tokenReader.heldBytes() <= maxPreambleBytes);
}

UBIQUITTY_TEST_MAIN()
