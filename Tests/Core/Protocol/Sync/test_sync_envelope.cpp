// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_envelope.cpp — the Sync envelope and the connection-plane bodies.
//
// No frame carries more than 65,535 body bytes, in either direction: the
// writer refuses one and the reader stops on a header that announces one,
// before it buffers the body. A reader stops for good on an unknown kind or a
// kind on the wrong channel scope, reassembles frames split at any byte, and
// takes a datagram only when it is exactly one envelope. Every body decoder
// refuses a body cut short or carrying a trailing byte.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Protocol/Sync/WireVersion.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

std::vector<std::uint8_t> frameOf(FrameKind kind, std::uint32_t channel, const std::vector<std::uint8_t>& body)
{
    std::vector<std::uint8_t> bytes;
    CHECK(appendFrame(bytes, kind, channel, body));
    return bytes;
}

/// A header announcing `length` body bytes, with none of them.
std::vector<std::uint8_t> headerAnnouncing(FrameKind kind, std::uint32_t channel, std::uint64_t length)
{
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.varint(channel);
    writer.varint(length);
    return bytes;
}

Greet sampleGreet()
{
    Greet greet;
    greet.wireVersion = wireVersion;
    greet.appVersion = "1.2.3";
    greet.engineFingerprint.fill(0xAB);
    greet.clientInstance = { .low = 0x0102030405060708, .high = 0x1112131415161718 };
    greet.clientKind = ClientKind::Agent;
    greet.requestedRole = Ubiquitty::Protocol::Sync::Role::Viewer;
    greet.facts = { .viewportRows = 24, .viewportColumns = 80, .cellPixelWidth = 9, .cellPixelHeight = 18, .textAreaPixelWidth = 720, .textAreaPixelHeight = 432, .refreshIntervalMicroseconds = 16'667, .link = LinkCost::Metered, .power = PowerState::Saver, .isFocused = true };
    greet.historyCacheLines = 0x01020304;
    greet.backfillDepthLines = 0x05060708;
    return greet;
}

} // namespace

// Every frame kind the reader accepts has a value of its own, and the reader
// accepts exactly those: two kinds sharing a number would read one as the
// other. `frameKinds` is the one list of the numbers, and its own assertion
// fails the build on a shared one; here it is checked against the reader byte
// by byte, so a kind the reader accepts and the list leaves out fails the
// count.
TEST(everyFrameKindHasAValueOfItsOwn)
{
    const std::span<const FrameKind> kinds = frameKinds;
    std::set<std::uint8_t> values;
    for (const FrameKind kind : kinds)
        CHECK(values.insert(std::to_underlying(kind)).second);
    std::size_t accepted = 0;
    for (unsigned value = 0; value < 256; ++value) {
        const std::optional<FrameKind> kind = frameKindOf(static_cast<std::uint8_t>(value));
        if (!kind)
            continue;
        ++accepted;
        CHECK(values.contains(static_cast<std::uint8_t>(value)));
    }
    CHECK_EQ(accepted, kinds.size());
}

// A Raw keeps its number and bytes, and a body one byte short or long is no
// Raw at all.
TEST(aRawRoundTripsAndRefusesAnyOtherLength)
{
    const std::array<std::uint8_t, 3> bytes { 'l', 's', '\r' };
    std::vector<std::uint8_t> body;
    encode(body, Raw { .inputSeq = 1ull << 40, .bytes = bytes });
    const std::optional<Raw> raw = decodeRaw(body);
    REQUIRE(raw.has_value());
    CHECK_EQ(raw->inputSeq, 1ull << 40);
    CHECK(std::ranges::equal(raw->bytes, bytes));
    CHECK(laneOf(FrameKind::Raw) == Lane::Interactive);
    CHECK(directionOf(FrameKind::Raw) == Direction::ClientToHost);
    CHECK(!isConnectionKind(FrameKind::Raw));
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeRaw(std::span<const std::uint8_t>(body).first(length)));
    body.push_back(0);
    CHECK(!decodeRaw(body));
}

// A Greet's resume and a Welcome's outcomes round-trip, and a pane list or an
// outcome the grammar does not allow is refused.
TEST(aResumeRoundTripsAndAnOverlongPaneListIsRefused)
{
    PaneResume pane;
    pane.paneID = { 9, 10 };
    pane.ackedGeneration = 1ull << 40;
    pane.ackedRowHashesDigest = Digest128 { .low = 7, .high = 8 };
    pane.historyEpoch = 3;
    pane.cachedRanges = { { .from = 10, .to = 20 }, { .from = 30, .to = 31 } };
    pane.lastInputSeqSent = 42;
    pane.effectsAcknowledged = 43;
    pane.view = ViewState::Dormant;
    for (const std::optional<ID128> secret : { std::optional<ID128> { ID128 { 5, 6 } }, std::optional<ID128> {} }) {
        Greet greet = sampleGreet();
        greet.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = secret, .panes = { pane, pane } };
        std::vector<std::uint8_t> body;
        encode(body, greet);
        const std::optional<Greet> decoded = decodeGreet(body);
        REQUIRE(decoded.has_value());
        CHECK(decoded->resume == greet.resume);
    }

    // An absent resume still writes its mandatory presence flag.
    std::vector<std::uint8_t> fresh;
    encode(fresh, sampleGreet());
    Greet withNone = sampleGreet();
    withNone.resume = std::nullopt;
    std::vector<std::uint8_t> same;
    encode(same, withNone);
    CHECK(fresh == same);
    CHECK(!decodeGreet(fresh)->resume.has_value());

    // 257 panes is past the cap, refused before any pane is read.
    Greet crowded = sampleGreet();
    crowded.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = std::nullopt, .panes = std::vector<PaneResume>(maxPanesPerConnection + 1, pane) };
    std::vector<std::uint8_t> crowdedBody;
    encode(crowdedBody, crowded);
    CHECK(!decodeGreet(crowdedBody));

    // A view the grammar does not know, or ranges out of order, is refused.
    Greet unknownView = sampleGreet();
    unknownView.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = std::nullopt, .panes = { pane } };
    unknownView.resume->panes.front().view = static_cast<ViewState>(9);
    std::vector<std::uint8_t> viewBody;
    encode(viewBody, unknownView);
    CHECK(!decodeGreet(viewBody));
    PaneResume unordered = pane;
    unordered.cachedRanges = { { .from = 30, .to = 31 }, { .from = 10, .to = 20 } };
    Greet unorderedGreet = sampleGreet();
    unorderedGreet.resume = ResumeRequest { .attachmentID = { 3, 4 }, .resumeSecret = std::nullopt, .panes = { unordered } };
    std::vector<std::uint8_t> unorderedBody;
    encode(unorderedBody, unorderedGreet);
    CHECK(!decodeGreet(unorderedBody));

    // The Welcome's outcomes, an absent inputWritten kept absent.
    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.appVersion = "1.2.3";
    welcome.selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 3, .daemonInstanceID = 7, .socketName = "host-test.sock", .buildFlags = 0, .hostPlatform = "linux" };
    welcome.resumed = { { .paneID = { 9, 10 }, .outcome = ResumeOutcome::Diffed, .inputWritten = 17 }, { .paneID = { 11, 12 }, .outcome = ResumeOutcome::Gone, .inputWritten = std::nullopt } };
    std::vector<std::uint8_t> welcomeBody;
    encode(welcomeBody, welcome);
    const std::optional<Welcome> decodedWelcome = decodeWelcome(welcomeBody);
    REQUIRE(decodedWelcome.has_value());
    CHECK(decodedWelcome->resumed == welcome.resumed);
    CHECK(!decodedWelcome->predecessorHostInstanceID.has_value());
    // An outcome the grammar does not know is refused: the second pane's
    // outcome byte sits before its one-byte absent inputWritten.
    welcomeBody[welcomeBody.size() - 3] = 7;
    CHECK(!decodeWelcome(welcomeBody));
}

TEST(aCanonicalByeNeverChangesItsReasonForAnOlderGrammar)
{
    for (const ByeReason reason : { ByeReason::Superseded, ByeReason::ResumeInProgress, ByeReason::Suspending }) {
        const Bye given = spokenBye(wireVersion, "1.2.3", "linux", reason, "moved to the phone");
        CHECK(given.reason == reason);
        CHECK(given.detail == "moved to the phone");
        REQUIRE(given.peer);
        CHECK(given.peer->build.wireVersion == wireVersion);
        CHECK(given.peer->build.appVersion == "1.2.3");
        CHECK(given.peer->platform == "linux");
    }
    std::vector<std::uint8_t> body;
    encode(body, Bye { .reason = ByeReason::Suspending, .detail = "asleep" });
    CHECK(decodeBye(body).has_value() && decodeBye(body)->reason == ByeReason::Suspending);
}

TEST(aPlannedHandoverHasAnExplicitWireVersionAndExactReason)
{
    CHECK(spokenBye(wireVersion, "1.2.3", "linux", ByeReason::HandingOver, {}).reason == ByeReason::HandingOver);
    std::vector<std::uint8_t> body;
    encode(body, Bye { .reason = ByeReason::HandingOver, .detail = {} });
    CHECK(body == std::vector<std::uint8_t>({ 10, 0, 0 }));
    REQUIRE(decodeBye(body));
    CHECK(decodeBye(body)->reason == ByeReason::HandingOver);
    body[0] = std::to_underlying(lastByeReasonOnWire) + 1;
    CHECK(!decodeBye(body));
}

TEST(onlyAnExactPredecessorContinuesAnotherHostInstance)
{
    const ID128 held { 21, 22 };
    const ID128 welcomed { 23, 24 };
    CHECK(continuityOf(held, held, std::nullopt) == InstanceContinuity::Same);
    CHECK(continuityOf(held, welcomed, held) == InstanceContinuity::HandedOver);
    CHECK(continuityOf(held, welcomed, std::nullopt) == InstanceContinuity::Restarted);
    CHECK(continuityOf(held, welcomed, ID128 { 25, 26 }) == InstanceContinuity::Restarted);
    CHECK(continuityOf({}, welcomed, ID128 {}) == InstanceContinuity::Restarted);
}

TEST(aBodyOverTheLimitIsNeverWritten)
{
    std::vector<std::uint8_t> output;
    CHECK(!appendFrame(output, FrameKind::Ack, 0, std::vector<std::uint8_t>(maxFrameBodyBytes + 1)));
    CHECK(output.empty());
    CHECK(appendFrame(output, FrameKind::Ack, 0, std::vector<std::uint8_t>(maxFrameBodyBytes)));
    CHECK_EQ(output.size(), frameBytes(0, maxFrameBodyBytes));
}

TEST(aHeaderAnnouncingMoreThanTheLimitStopsTheReaderBeforeTheBody)
{
    FrameDecoder decoder;
    decoder.append(headerAnnouncing(FrameKind::Update, 1, maxFrameBodyBytes + 1));
    CHECK(!decoder.next());
    CHECK(decoder.error() == EnvelopeError::Oversize);
    // Nothing after the failure is buffered, so a hostile length cannot make
    // the reader hold its body.
    decoder.append(std::vector<std::uint8_t>(maxFrameBodyBytes + 1));
    CHECK(!decoder.next());
    CHECK(decoder.bufferedBytes() < maxEnvelopeHeaderBytes);

    // A length that needs a fourth varint group is past the cap however it ends.
    FrameDecoder longLength;
    longLength.append(std::vector<std::uint8_t> { static_cast<std::uint8_t>(FrameKind::Update), 1, 0x80, 0x80, 0x80 });
    CHECK(!longLength.next());
    CHECK(longLength.error() == EnvelopeError::Oversize);

    FrameDecoder atLimit;
    atLimit.append(headerAnnouncing(FrameKind::Update, 1, maxFrameBodyBytes));
    atLimit.append(std::vector<std::uint8_t>(maxFrameBodyBytes));
    const std::optional<Frame> frame = atLimit.next();
    CHECK(frame && frame->body.size() == maxFrameBodyBytes);
}

TEST(anUnknownKindOrAWrongScopeStopsTheReader)
{
    FrameDecoder unknown;
    unknown.append(std::vector<std::uint8_t> { 0xEE, 0, 0 });
    CHECK(!unknown.next());
    CHECK(unknown.error() == EnvelopeError::UnknownKind);

    // A pane kind on the connection channel, and a connection kind on a pane.
    FrameDecoder paneOnConnection;
    paneOnConnection.append(headerAnnouncing(FrameKind::Update, 0, 0));
    CHECK(!paneOnConnection.next());
    CHECK(paneOnConnection.error() == EnvelopeError::Malformed);
    FrameDecoder connectionOnPane;
    connectionOnPane.append(headerAnnouncing(FrameKind::Ack, 3, 0));
    CHECK(!connectionOnPane.next());
    CHECK(connectionOnPane.error() == EnvelopeError::Malformed);

    std::vector<std::uint8_t> output;
    CHECK(!appendFrame(output, FrameKind::Update, 0, {}));
    CHECK(!appendFrame(output, FrameKind::Greet, 1, {}));
    CHECK(output.empty());

    // An overlong channel: a zero final group spells 1 at greater length.
    FrameDecoder overlong;
    overlong.append(std::vector<std::uint8_t> { static_cast<std::uint8_t>(FrameKind::Update), 0x81, 0x00, 0 });
    CHECK(!overlong.next());
    CHECK(overlong.error() == EnvelopeError::Malformed);
}

TEST(framesSplitAtEveryByteReassemble)
{
    std::vector<std::uint8_t> stream;
    for (std::uint32_t index = 0; index < 5; ++index) {
        const std::vector<std::uint8_t> body(index * 300, static_cast<std::uint8_t>(index));
        const std::vector<std::uint8_t> frame = frameOf(index % 2 ? FrameKind::Update : FrameKind::Ack, index % 2 ? index : 0, body);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    FrameDecoder decoder;
    std::uint32_t seen = 0;
    for (const std::uint8_t byte : stream) {
        decoder.append(std::vector<std::uint8_t> { byte });
        while (const std::optional<Frame> frame = decoder.next()) {
            CHECK_EQ(frame->body.size(), seen * 300);
            CHECK_EQ(frame->channel, seen % 2 ? seen : 0u);
            ++seen;
        }
    }
    CHECK_EQ(seen, 5u);
    CHECK(decoder.error() == EnvelopeError::None);
    CHECK_EQ(decoder.bufferedBytes(), 0u);
}

TEST(aDatagramIsExactlyOneEnvelope)
{
    std::vector<std::uint8_t> datagram = frameOf(FrameKind::Update, 4, std::vector<std::uint8_t>(20, 7));
    EnvelopeError error = EnvelopeError::None;
    const std::optional<Frame> frame = decodeDatagram(datagram, error);
    CHECK(frame && frame->kind == FrameKind::Update && frame->channel == 4 && frame->body.size() == 20);

    datagram.push_back(0);
    CHECK(!decodeDatagram(datagram, error));
    CHECK(error == EnvelopeError::Malformed);
    datagram.resize(datagram.size() - 2);
    CHECK(!decodeDatagram(datagram, error));
    CHECK(error == EnvelopeError::Malformed);
}

// Every connection body refuses one cut short. A byte past the last field is
// refused too, except on the handshake, which ignores what a newer peer
// appends (Docs/sync.md §7.3, "Versions").
TEST(connectionBodiesRoundTripAndRefuseAnyOtherLength)
{
    std::vector<std::uint8_t> body;
    encode(body, sampleGreet());
    const std::optional<Greet> greet = decodeGreet(body);
    CHECK(greet && greet->wireVersion == wireVersion && greet->clientInstance == sampleGreet().clientInstance);
    CHECK(greet && greet->facts == sampleGreet().facts && greet->engineFingerprint == sampleGreet().engineFingerprint);
    CHECK(greet && greet->historyCacheLines == sampleGreet().historyCacheLines && greet->backfillDepthLines == sampleGreet().backfillDepthLines);
    CHECK(greet && greet->clientKind == ClientKind::Agent && greet->requestedRole == Ubiquitty::Protocol::Sync::Role::Viewer);
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeGreet(std::span<const std::uint8_t>(body).first(length)));
    body.push_back(0);
    CHECK(!decodeGreet(body));

    Ack ack { .hostTimeEcho = 123'456, .holdMicroseconds = 789, .bytesReceived = 1ull << 40, .panes = { { .handle = 1, .generation = 9 }, { .handle = 300, .generation = 1ull << 50 } } };
    body.clear();
    encode(body, ack);
    const std::optional<Ack> decodedAck = decodeAck(body);
    CHECK(decodedAck && decodedAck->hostTimeEcho == 123'456 && decodedAck->holdMicroseconds == 789 && decodedAck->bytesReceived == 1ull << 40);
    CHECK(decodedAck && decodedAck->panes.size() == 2 && decodedAck->panes[1].handle == 300 && decodedAck->panes[1].generation == 1ull << 50);
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeAck(std::span<const std::uint8_t>(body).first(length)));
    body.push_back(0);
    CHECK(!decodeAck(body));

    // A count promising more panes than the body holds is refused before any
    // element is read.
    body.clear();
    WireWriter writer(body);
    writer.u64(1);
    writer.u32(0);
    writer.u64(0);
    writer.varint(1'000'000);
    CHECK(!decodeAck(body));

    Resync resync { .reason = ResyncReason::BaseMissing, .generation = 77, .sectionHashes = { 1, 2, 3 } };
    body.clear();
    encode(body, resync);
    const std::optional<Resync> decodedResync = decodeResync(body);
    CHECK(decodedResync && decodedResync->reason == ResyncReason::BaseMissing && decodedResync->generation == 77 && decodedResync->sectionHashes == resync.sectionHashes);
    body[0] = 0x7F;
    CHECK(!decodeResync(body));

    const std::vector<std::uint8_t> data(1000, 0x5A);
    BulkChunk chunk { .transfer = 3, .index = 4, .isLast = true, .bytes = data, .sha256 = {} };
    chunk.sha256.fill(0xC3);
    body.clear();
    encode(body, chunk);
    const std::optional<BulkChunk> decodedChunk = decodeBulkChunk(body);
    CHECK(decodedChunk && decodedChunk->transfer == 3 && decodedChunk->index == 4 && decodedChunk->isLast);
    CHECK(decodedChunk && decodedChunk->bytes.size() == 1000 && decodedChunk->sha256 == chunk.sha256);
    body.pop_back();
    CHECK(!decodeBulkChunk(body));

    // An update moves forward: a target at or behind its base is refused.
    body.clear();
    encode(body, UpdateHeader { .mode = UpdateMode::Chain, .baseGeneration = 5, .generation = 5, .frameClass = FrameClass::Settled, .hostTime = 1 });
    CHECK(!decodeUpdateHeader(body));
}

TEST(aVersionMismatchNamesNoSide)
{
    // The handshake carries both builds and Session composes the sentence that
    // names the older one; the wire's own notice guesses at neither.
    CHECK(byeNotice(ByeReason::VersionMismatch) == versionMismatchNotice());
    CHECK(versionMismatchNotice().find("whichever") == std::string_view::npos);
    Bye bye { .reason = ByeReason::VersionMismatch, .detail = std::string(versionMismatchNotice()) };
    std::vector<std::uint8_t> body;
    encode(body, bye);
    const std::optional<Bye> decoded = decodeBye(body);
    CHECK(decoded && decoded->reason == ByeReason::VersionMismatch && decoded->detail == bye.detail);
}

TEST(aRefusalIsTerminalUnlessTryingAgainCanChangeIt)
{
    // Which reasons are a host declining to serve, and which of those a
    // client stops on. Every reason is named, so a new one is placed here.
    struct Row {
        ByeReason reason;
        bool isRefusal; // bool-ok: one reason's expected class
        bool isPermanent; // bool-ok: one reason's expected class
    };
    constexpr Row rows[] = {
        { ByeReason::Normal, false, false },
        { ByeReason::VersionMismatch, true, true },
        { ByeReason::Malformed, false, true },
        { ByeReason::Oversize, false, false },
        { ByeReason::UnknownKind, false, false },
        { ByeReason::UnboundHandle, false, false },
        { ByeReason::TransferRefused, false, false },
        { ByeReason::Superseded, false, false },
        { ByeReason::ResumeInProgress, false, false },
        { ByeReason::Suspending, false, false },
        { ByeReason::HandingOver, false, false },
        { ByeReason::TokenMissing, true, true },
        { ByeReason::TokenInvalid, true, true },
        { ByeReason::TokenExpired, true, true },
        { ByeReason::TokenRevoked, true, true },
        { ByeReason::OutsideScope, true, true },
        { ByeReason::DuplicateHandshake, true, false },
        { ByeReason::NoConsent, true, false },
        { ByeReason::RemoteHostRefused, true, true },
        { ByeReason::SessionGone, true, true },
        { ByeReason::HostFull, true, false },
        { ByeReason::OutputBacklog, false, false },
        { ByeReason::Silent, false, false },
    };
    std::set<std::string_view> notices;
    for (const Row& row : rows) {
        CHECK(isRefusal(row.reason) == row.isRefusal);
        CHECK(isPermanentRefusal(row.reason) == row.isPermanent);
        CHECK(!byeNotice(row.reason).empty() && byeNotice(row.reason) != "the connection closed");
        notices.insert(byeNotice(row.reason));
    }
    CHECK_EQ(std::size(rows), static_cast<std::size_t>(std::to_underlying(lastByeReasonOnWire)) + 1);
    CHECK_EQ(notices.size(), std::size(rows));
    // A Bye frame carries the reasons through `lastByeReasonOnWire` and no
    // refusal past it.
    std::vector<std::uint8_t> body;
    encode(body, Bye { .reason = lastByeReasonOnWire, .detail = {} });
    CHECK(decodeBye(body).has_value());
    body.clear();
    encode(body, Bye { .reason = ByeReason::TokenMissing, .detail = {} });
    CHECK(decodeBye(body).has_value());
    body[0] = std::to_underlying(lastByeReasonOnWire) + 1;
    CHECK(!decodeBye(body));
    // A QUIC application close carries every reason, and no code outside
    // the reasons' range is read as one.
    for (const Row& row : rows)
        CHECK(byeReasonOfQUICClose(quicCloseCodeOf(row.reason)) == std::optional { row.reason });
    CHECK(!byeReasonOfQUICClose(quicByeCloseCodeBase - 1));
    CHECK(!byeReasonOfQUICClose(quicCloseCodeOf(lastByeReasonOnWire) + 1));
    CHECK(!byeReasonOfQUICClose(0));
}

UBIQUITTY_TEST_MAIN()
