// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_connection.cpp — a host and a client connection over a simulated
// link.
//
// The host's terminal is driven by escape sequences, published generation by
// generation, and carried to the client's replica by the two state machines
// alone; the test moves their bytes, drops datagrams and withholds acks. The
// properties held:
//
// - A link that loses one datagram in ten never stalls the replica and never
//   asks for a checkpoint: the next superset update covers what was lost.
// - A client that skipped generations is brought up by one diff.
// - A corrupted update costs exactly one `Resync`, however many updates were
//   already in flight behind it, and the checkpoint it brings converges.
// - The host never has more unacknowledged bytes out than its window.
// - A peer at another wire version is refused by either end.
// - The round trip leaves out the time the client held its `Ack`.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncCorpus.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/RichText/TypographyFacts.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

using UbiquittyTest::SyncLink;

constexpr std::size_t maxDatagramBytes = UbiquittyTest::syncMaxDatagramBytes;

HostPolicy hostPolicy(std::size_t datagramMaxBytes)
{
    return UbiquittyTest::syncHostPolicy(datagramMaxBytes);
}

constexpr ClientPolicy clientPolicy = UbiquittyTest::syncClientPolicy;

constexpr std::uint64_t frameInterval = UbiquittyTest::syncFrameInterval;

/// A redraw of one row and the status line, as a ticker or a progress bar
/// writes them. Each frame names a colour and an emoji sequence, so pens and
/// pool clusters are defined while earlier updates are still unacknowledged.
std::string tickerFrame(int index)
{
    static constexpr std::array<std::string_view, 5> professions = { "\xf0\x9f\x92\xbb", "\xf0\x9f\x94\xac", "\xf0\x9f\x9a\x80", "\xf0\x9f\x8e\xa8", "\xf0\x9f\x8d\xb3" };
    const std::string worker = std::string("\xf0\x9f\x91\xa8\xe2\x80\x8d") + std::string(professions[static_cast<std::size_t>(index / 7 % 5)]);
    return "\x1b[" + std::to_string(2 + index % 20) + ";1H\x1b[2K\x1b[38;5;" + std::to_string(index % 200) + "mline " + std::to_string(index) + " " + worker
        + "\x1b[0m\x1b[24;1H\x1b[2K[" + std::to_string(index * 7) + "]";
}

template<typename Message>
std::vector<std::uint8_t> frameOf(FrameKind kind, std::uint32_t channel, const Message& message)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    std::vector<std::uint8_t> frame;
    (void)appendFrame(frame, kind, channel, body);
    return frame;
}

std::vector<std::uint8_t> chunkFrame(std::uint32_t transfer, std::uint32_t index, std::size_t bytes)
{
    const std::vector<std::uint8_t> data(bytes, 0x5A);
    return frameOf(FrameKind::BulkChunk, 0, BulkChunk { .transfer = transfer, .index = index, .isLast = false, .bytes = data, .sha256 = {} });
}

} // namespace

TEST(aHostWelcomeCarriesItsVerifiedPredecessor)
{
    HostIdentity identity;
    identity.selection = UbiquittyTest::syncDefaultSelection();
    identity.hostInstanceID = { 51, 52 };
    identity.predecessorHostInstanceID = ID128 { 41, 42 };
    HostConnection host(hostPolicy(0), identity);
    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    host.receiveStream(client.takeStreamOutput(), 1);
    std::vector<std::uint8_t> stream;
    while (host.popStreamFrame(stream, 1)) {
    }
    client.receiveStream(stream, 1);
    REQUIRE(client.welcome());
    CHECK(client.welcome()->hostInstanceID == identity.hostInstanceID);
    CHECK(client.welcome()->predecessorHostInstanceID == identity.predecessorHostInstanceID);
}

TEST(aHandoverByeClosesTheConnectionWithoutEndingTheReplica)
{
    SyncLink link(maxDatagramBytes, 24);
    link.write("kept across handover\r\n");
    link.step();
    const auto* picture = link.client().paneTerminal(link.handle());
    REQUIRE(picture);
    link.client().receiveStream(frameOf(FrameKind::Bye, 0, Bye { .reason = ByeReason::HandingOver, .detail = {} }), 1);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::HandingOver);
    CHECK(link.client().paneTerminal(link.handle()) == picture);
    CHECK(link.disagreement(link.handle()).empty());
}

TEST(tenPercentDatagramLossNeverStallsAndNeverAsksForACheckpoint)
{
    SyncLink link(maxDatagramBytes, 24);
    link.setLossPermille(100);
    std::uint64_t worstLag = 0;
    for (int index = 0; index < 600; ++index) {
        link.write(tickerFrame(index));
        link.step();
        const std::uint64_t lag = link.newest() - link.held();
        // A lost update is covered by the next one, so the replica trails by
        // the run of losses it is in, and never waits out a timeout.
        CHECK_MSG(lag <= link.longestLossRun() + 1, ("frame " + std::to_string(index) + " trails by " + std::to_string(lag)).c_str());
        worstLag = std::max(worstLag, lag);
    }
    CHECK_MSG(link.datagramsDropped() >= 40, "the link lost too few datagrams to test anything");
    CHECK(link.host().counters().supersetUpdatesSent > 500);
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 0 });
    CHECK_EQ(link.client().counters().digestMismatches, std::uint64_t { 0 });

    // The last update may have been lost with nothing behind it; the loss
    // timeout resends, so the replica converges once the link is clean.
    link.setLossPermille(0);
    for (int index = 0; index < 10; ++index)
        link.step();
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });
}

TEST(aClientThatSkippedGenerationsGetsADiffNeverACheckpoint)
{
    for (const std::size_t datagramMaxBytes : { std::size_t { 0 }, maxDatagramBytes }) {
        SyncLink link(datagramMaxBytes, 24);
        const std::uint64_t chainBefore = link.host().counters().chainUpdatesSent;
        const std::uint64_t supersetBefore = link.host().counters().supersetUpdatesSent;
        // Five generations, the fourth scrolling the screen, before the host
        // services the attachment again.
        for (int index = 0; index < 5; ++index) {
            link.write(index == 3 ? std::string("\x1b[24;1H\n\n\nscrolled") : tickerFrame(index));
        }
        link.step();
        CHECK_EQ(link.held(), link.newest());
        CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });
        CHECK_EQ((link.host().counters().chainUpdatesSent - chainBefore) + (link.host().counters().supersetUpdatesSent - supersetBefore), std::uint64_t { 1 });
        const std::string difference = link.disagreement(link.handle());
        CHECK_MSG(difference.empty(), difference.c_str());
    }
}

TEST(aDigestMismatchTriggersExactlyOneResync)
{
    SyncLink link(0, 24);
    link.write(tickerFrame(0));
    link.step();
    link.step();

    // The marker row's text is corrupted in flight: the record still decodes,
    // so only the digest can tell.
    bool isCorrupted = false;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        const std::string_view marker = "QQQQQQ";
        const auto found = std::search(stream.begin(), stream.end(), marker.begin(), marker.end());
        if (found != stream.end() && !isCorrupted) {
            *found = 'R';
            isCorrupted = true;
        }
    });
    link.write("\x1b[5;1HQQQQQQ");
    // The Resync and every ack are held while three more updates follow the
    // corrupted one, so each arrives while the Resync is outstanding.
    link.holdUpstream();
    link.step();
    REQUIRE(isCorrupted);
    for (int index = 1; index <= 3; ++index) {
        link.write(tickerFrame(index));
        link.step();
    }
    CHECK_EQ(link.client().counters().digestMismatches, std::uint64_t { 1 });
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 1 });
    link.releaseUpstream();
    for (int index = 0; index < 4; ++index)
        link.step();
    CHECK_EQ(link.host().counters().resyncsReceived, std::uint64_t { 1 });
    // The host names the row that diverged, for the journal.
    CHECK_EQ(link.host().counters().lastResyncFirstRow, 4);
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 2 });
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 1 });
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(theHostNeverHasMoreThanItsWindowOutstanding)
{
    SyncLink link(0, 24);
    // The client stops acknowledging while every row of every generation changes.
    link.holdUpstream();
    const std::uint64_t updatesBefore = link.host().counters().chainUpdatesSent;
    for (int index = 0; index < 60; ++index) {
        std::string redraw = "\x1b[H";
        for (int row = 0; row < 24; ++row)
            redraw += "\x1b[" + std::to_string(row + 1) + ";1Hgeneration " + std::to_string(index) + " row " + std::to_string(row) + " " + std::string(40, static_cast<char>('a' + (index + row) % 26));
        link.write(redraw);
        link.step();
        const std::uint64_t outstanding = link.host().pacer().outstandingBytes();
        CHECK_MSG(outstanding <= link.host().pacer().windowBytes(link.now()),
            ("frame " + std::to_string(index) + ": " + std::to_string(outstanding) + " bytes outstanding against a window of " + std::to_string(link.host().pacer().windowBytes(link.now()))).c_str());
    }
    CHECK(link.host().counters().windowRefusals > 0);
    const std::uint64_t sentWhileStalled = link.host().counters().chainUpdatesSent - updatesBefore;
    CHECK(sentWhileStalled < 20);

    // Acks resume: one fresh update goes from the sent generation to the
    // newest, not one per generation the stall skipped.
    link.releaseUpstream();
    link.step();
    link.step();
    CHECK_EQ(link.held(), link.newest());
    CHECK(link.host().counters().chainUpdatesSent - updatesBefore <= sentWhileStalled + 2);
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());

    // Every generation now switches screens, which only a checkpoint says:
    // checkpoints wait for the window as updates do.
    link.holdUpstream();
    const std::uint64_t checkpointsBefore = link.host().counters().checkpointsSent;
    for (int index = 0; index < 60; ++index) {
        link.write(std::string(index % 2 ? "\x1b[?1049l" : "\x1b[?1049h") + "screen " + std::to_string(index) + std::string(1500, '#'));
        link.step();
        CHECK(link.host().pacer().outstandingBytes() <= link.host().pacer().windowBytes(link.now()));
    }
    CHECK(link.host().counters().checkpointsSent - checkpointsBefore < 20);
    link.releaseUpstream();
    for (int index = 0; index < 6; ++index)
        link.step();
    CHECK_EQ(link.held(), link.newest());
    const std::string afterSwitches = link.disagreement(link.handle());
    CHECK_MSG(afterSwitches.empty(), afterSwitches.c_str());
}

TEST(theRoundTripLeavesOutTheTimeTheClientHeldItsAck)
{
    // The link delivers instantly, and the client holds each Ack until the
    // frame interval after the update it answers, so a round trip that
    // counted the hold would read 16 ms.
    SyncLink link(maxDatagramBytes, 24);
    for (int index = 0; index < 30; ++index) {
        link.write(tickerFrame(index));
        link.step();
        link.step();
    }
    const std::optional<std::uint64_t> roundTrip = link.host().pacer().smoothedRoundTripMicroseconds();
    REQUIRE(roundTrip);
    CHECK_MSG(*roundTrip < 1'000, ("the round trip read " + std::to_string(*roundTrip) + " us").c_str());

    // Idle past the probe interval: a probe goes and its echo is a sample.
    const std::uint64_t probesBefore = link.host().counters().probesSent;
    for (int index = 0; index < 130; ++index)
        link.step();
    CHECK(link.host().counters().probesSent > probesBefore);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(*link.host().pacer().smoothedRoundTripMicroseconds() < 1'000);
}

std::vector<std::uint8_t> greetFrame(std::uint16_t version, const Facts& facts = {})
{
    Greet greet;
    greet.clientInstance = { 7, 8 };
    greet.wireVersion = version;
    greet.facts = facts;
    std::vector<std::uint8_t> body;
    encode(body, greet);
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Greet, 0, body));
    return frame;
}

std::vector<std::uint8_t> welcomeFrame(std::uint16_t version, const EngineFingerprint& fingerprint = wireFingerprint, std::string_view appVersion = {})
{
    Welcome welcome;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    welcome.wireVersion = version;
    welcome.engineFingerprint = fingerprint;
    welcome.appVersion = std::string(appVersion);
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Welcome, 0, body));
    return frame;
}

/// One frame a host queued, its body copied out of the decoder.
struct QueuedFrame {
    FrameKind kind = FrameKind::Bye;
    std::vector<std::uint8_t> body;
};

/// Every frame a host has queued, decoded; `storage` receives the raw bytes.
std::vector<QueuedFrame> hostFrames(HostConnection& host, std::vector<std::uint8_t>& storage)
{
    storage.clear();
    while (host.popStreamFrame(storage, 1)) {
    }
    FrameDecoder decoder;
    decoder.append(storage);
    std::vector<QueuedFrame> frames;
    while (const std::optional<Frame> frame = decoder.next())
        frames.push_back({ .kind = frame->kind, .body = { frame->body.begin(), frame->body.end() } });
    return frames;
}

// The wire has one version: a peer at any other is refused with
// VersionMismatch by either end, and no side is guessed older.
TEST(aPeerAtAnotherVersionIsRefusedAndNoSideIsGuessed)
{
    CHECK(wireVersion >= lastPublishedWireVersion);

    // The client refuses a Welcome from a host at another version.
    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    (void)client.takeStreamOutput();
    client.receiveStream(welcomeFrame(wireVersion + 1), 1);
    CHECK(client.state() == ConnectionState::Closed);
    CHECK(client.byeReason() == ByeReason::VersionMismatch);
    CHECK(!client.welcome());
    CHECK(client.notice().find("whichever") == std::string_view::npos);
    FrameDecoder clientOutput;
    clientOutput.append(client.takeStreamOutput());
    const std::optional<Frame> clientBye = clientOutput.next();
    REQUIRE(clientBye);
    CHECK(clientBye->kind == FrameKind::Bye);
    const std::optional<Bye> clientByeBody = decodeBye(clientBye->body);
    CHECK(clientByeBody && clientByeBody->reason == ByeReason::VersionMismatch && clientByeBody->detail == versionMismatchNotice());

    // The host refuses a Greet at any other version, and welcomes nobody.
    for (const std::uint16_t version : { std::uint16_t { wireVersion - 1 }, std::uint16_t { wireVersion + 1 } }) {
        HostConnection host(hostPolicy(0), HostIdentity {});
        host.receiveStream(greetFrame(version), 1);
        CHECK(host.state() == ConnectionState::Closed);
        CHECK(host.byeReason() == ByeReason::VersionMismatch);
        std::vector<std::uint8_t> hostOutput;
        const std::vector<QueuedFrame> refusal = hostFrames(host, hostOutput);
        REQUIRE(refusal.size() == 1);
        CHECK(refusal[0].kind == FrameKind::Bye);
        // The refusal carries the host's own version for the sentence a person reads.
        const std::optional<Bye> hostBye = decodeBye(refusal[0].body);
        REQUIRE(hostBye && hostBye->peer);
        CHECK_EQ(hostBye->peer->build.wireVersion, wireVersion);

        // The client hears the host's refusal as the same sentence.
        ClientConnection refused(clientPolicy, ClientIdentity {});
        refused.start();
        refused.receiveStream(hostOutput, 1);
        CHECK(refused.byeReason() == ByeReason::VersionMismatch);
        CHECK(refused.notice() == versionMismatchNotice());
    }

    // Version 0 names no version: the Greet is malformed rather than skewed.
    HostConnection unversioned(hostPolicy(0), HostIdentity {});
    unversioned.receiveStream(greetFrame(0), 1);
    CHECK(unversioned.byeReason() == ByeReason::Malformed);
}

// A build that changes its layout without advancing the wire version is
// refused for its fingerprint, from the handshake's prefix and
// before anything past it is read: the pair never gets as far as a body one
// of them cannot decode. The refusal carries the refusing end's build, so the
// sentence a person reads can name both without guessing which is older.
TEST(aBuildWhoseLayoutsDifferIsRefusedAtTheHandshakeByEitherEnd)
{
    EngineFingerprint other = wireFingerprint;
    other[0] ^= 0xFF;

    // The host refuses a client of another build and welcomes nobody.
    HostIdentity identity;
    identity.selection = UbiquittyTest::syncDefaultSelection();
    identity.appVersion = "0.2.0.7";
    HostConnection host(hostPolicy(0), identity);
    ClientIdentity olderClient;
    olderClient.engineFingerprint = other;
    olderClient.appVersion = "0.2.0.3";
    ClientConnection client(clientPolicy, olderClient);
    client.start();
    host.receiveStream(client.takeStreamOutput(), 1);
    CHECK(host.state() == ConnectionState::Closed);
    CHECK(host.byeReason() == ByeReason::VersionMismatch);
    std::vector<std::uint8_t> hostOutput;
    const std::vector<QueuedFrame> refusal = hostFrames(host, hostOutput);
    REQUIRE(refusal.size() == 1);
    CHECK(refusal[0].kind == FrameKind::Bye);
    const std::optional<Bye> bye = decodeBye(refusal[0].body);
    REQUIRE(bye && bye->peer);
    CHECK(bye->reason == ByeReason::VersionMismatch);
    CHECK_EQ(bye->peer->build.wireVersion, wireVersion);
    CHECK(bye->peer->build.appVersion == "0.2.0.7");
    client.receiveStream(hostOutput, 1);
    CHECK(client.byeReason() == ByeReason::VersionMismatch);
    CHECK(!client.welcome());
    CHECK(client.notice() == versionMismatchNotice());
    CHECK(olderSideOfRefusal(ByeReason::VersionMismatch, bye->peer->build, PeerBuild { .isKnown = true, .wireVersion = wireVersion, .appVersion = "0.2.0.3" }) == OlderSide::Unknown);

    // A Greet whose body past the prefix this build cannot read is refused
    // for its build, not taken for a malformed one of this build.
    Greet greet;
    greet.wireVersion = wireVersion;
    greet.appVersion = "0.2.0.3";
    greet.engineFingerprint = other;
    std::vector<std::uint8_t> body;
    encode(body, greet);
    body.resize(2 + 1 + greet.appVersion.size() + greet.engineFingerprint.size());
    body.insert(body.end(), { 0xFF, 0xFF, 0xFF });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Greet, 0, body));
    HostConnection layoutHost(hostPolicy(0), identity);
    layoutHost.receiveStream(frame, 1);
    CHECK(layoutHost.byeReason() == ByeReason::VersionMismatch);

    // The client refuses a host of another build, and keeps what the host
    // said of its build for the sentence.
    ClientConnection newerClient(clientPolicy, ClientIdentity {});
    newerClient.start();
    (void)newerClient.takeStreamOutput();
    newerClient.receiveStream(welcomeFrame(wireVersion, other, "0.2.0.3"), 1);
    CHECK(newerClient.state() == ConnectionState::Closed);
    CHECK(newerClient.byeReason() == ByeReason::VersionMismatch);
    CHECK(!newerClient.welcome());
    REQUIRE(newerClient.refusedHostBuild());
    CHECK(newerClient.refusedHostBuild()->appVersion == "0.2.0.3");
    CHECK(olderSideOfRefusal(ByeReason::VersionMismatch, *newerClient.refusedHostBuild(), PeerBuild { .isKnown = true, .wireVersion = wireVersion, .appVersion = "0.2.0.7" }) == OlderSide::Unknown);

    // The same build is welcomed, and a client refuses no Welcome of its own build.
    ClientConnection sameClient(clientPolicy, ClientIdentity {});
    sameClient.start();
    (void)sameClient.takeStreamOutput();
    sameClient.receiveStream(welcomeFrame(wireVersion), 1);
    CHECK(sameClient.state() == ConnectionState::Attached);
    CHECK(!sameClient.refusedHostBuild());
}

TEST(aPeerAtThisVersionIsWelcomedAtIt)
{
    HostIdentity identity;
    identity.selection = UbiquittyTest::syncDefaultSelection();
    HostConnection host(hostPolicy(0), identity);
    host.receiveStream(greetFrame(wireVersion), 1);
    REQUIRE(host.state() == ConnectionState::Attached);
    std::vector<std::uint8_t> output;
    const std::vector<QueuedFrame> frames = hostFrames(host, output);
    REQUIRE(!frames.empty());
    REQUIRE(frames[0].kind == FrameKind::Welcome);
    const std::optional<Welcome> welcome = decodeWelcome(frames[0].body);
    REQUIRE(welcome);
    CHECK_EQ(welcome->wireVersion, wireVersion);

    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    (void)client.takeStreamOutput();
    client.receiveStream(output, 1);
    CHECK(client.state() == ConnectionState::Attached);
}

// A build at version 2 meets this one: each end refuses the other, and the
// host's refusal names the version it speaks.
TEST(aBuildAtVersionTwoAndABuildAtVersionOneRefuseEachOther)
{
    HostIdentity newer;
    newer.selection = UbiquittyTest::syncDefaultSelection();
    newer.wireVersion = 2;
    HostConnection host(hostPolicy(0), newer);
    host.receiveStream(greetFrame(1), 1);
    CHECK(host.byeReason() == ByeReason::VersionMismatch);
    std::vector<std::uint8_t> hostOutput;
    const std::vector<QueuedFrame> refusal = hostFrames(host, hostOutput);
    REQUIRE(refusal.size() == 1);
    const std::optional<Bye> bye = decodeBye(refusal[0].body);
    REQUIRE(bye && bye->peer);
    CHECK_EQ(bye->peer->build.wireVersion, 2);

    ClientIdentity newerClient;
    newerClient.wireVersion = 2;
    ClientConnection client(clientPolicy, newerClient);
    client.start();
    FrameDecoder greet;
    greet.append(client.takeStreamOutput());
    const std::optional<Frame> greetFrameRead = greet.next();
    REQUIRE(greetFrameRead && greetFrameRead->kind == FrameKind::Greet);
    CHECK(decodeHandshakeVersion(greetFrameRead->body) == std::optional<std::uint16_t> { 2 });
    client.receiveStream(welcomeFrame(1), 1);
    CHECK(client.byeReason() == ByeReason::VersionMismatch);
}

TEST(aFrameFromTheWrongPeerOrBeforeTheHandshakeClosesTheConnection)
{
    HostConnection host(hostPolicy(0), HostIdentity {});
    std::vector<std::uint8_t> body;
    encode(body, Ack {});
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Ack, 0, body));
    host.receiveStream(frame, 1);
    CHECK(host.state() == ConnectionState::Closed);
    CHECK(host.byeReason() == ByeReason::Malformed);

    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    body.clear();
    encode(body, Greet {});
    frame.clear();
    REQUIRE(appendFrame(frame, FrameKind::Greet, 0, body));
    client.receiveStream(frame, 1);
    CHECK(client.state() == ConnectionState::Closed);
    CHECK(client.byeReason() == ByeReason::Malformed);
}

// The body of a message, encoded as it travels.
template<typename Message>
std::vector<std::uint8_t> bodyOf(const Message& message)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    return body;
}

// A client never receives a want, and a host never receives the frames it
// alone sends: each closes the connection as malformed.
TEST(aHistoryFrameFromTheWrongPeerClosesTheConnection)
{
    SyncLink link(0, 24);
    link.pump();
    REQUIRE(link.client().state() == ConnectionState::Attached);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::HistoryWant, link.handle(), bodyOf(HistoryWant { .historyEpoch = 1, .ranges = { { 0, 1 } }, .priority = HistoryPriority::Viewport })));
    link.deliverToClient(frame);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);

    SyncLink other(0, 24);
    other.pump();
    REQUIRE(other.host().state() == ConnectionState::Attached);
    other.sendToHost(FrameKind::HistoryReset, bodyOf(HistoryReset { .newEpoch = 2, .clearedThrough = 0, .historyHead = 0 }), other.handle());
    CHECK(other.host().state() == ConnectionState::Closed);
    CHECK(other.host().byeReason() == ByeReason::Malformed);
}

// A host reads a client's want and keeps the connection, and refuses one it
// cannot read.
TEST(aHostReadsAWantAndRefusesAnUnreadableOne)
{
    SyncLink link(0, 24);
    link.pump();
    link.sendToHost(FrameKind::HistoryWant, bodyOf(HistoryWant { .historyEpoch = 1, .ranges = { { 0, 1 } }, .priority = HistoryPriority::Viewport }), link.handle());
    CHECK(link.host().state() == ConnectionState::Attached);
    link.sendToHost(FrameKind::HistoryWant, bodyOf(HistoryWant { .historyEpoch = 1, .ranges = { { 5, 1 } }, .priority = HistoryPriority::Viewport }), link.handle());
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::Malformed);
}

// An update is encoded from the terminal as it is when it is sent, so a pane
// whose terminal moved on since its last frame sends nothing until the next
// publish: a frame between two coherent ones could be one the program has
// not finished drawing.
TEST(aPaneWhoseTerminalMovedOnWaitsForItsNextPublish)
{
    SyncLink link(0, 24);
    const std::uint64_t updatesBefore = link.host().counters().chainUpdatesSent;
    link.write("ready");
    link.writeUnpublished("\x1b[?25lhalf");
    link.step();
    CHECK_EQ(link.host().counters().chainUpdatesSent, updatesBefore);
    CHECK(link.held() < link.newest());
    link.publish();
    link.step();
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A pane unbound on the host tells the client, sends it nothing more, and an
// Ack the client sent before it learned does not end the connection.
TEST(anUnboundPaneSendsNothingAndForgivesALateAck)
{
    SyncLink link(0, 24);
    link.unbind();
    link.step();
    CHECK(link.client().paneTerminal(link.handle()) == nullptr);
    const std::uint64_t updatesBefore = link.host().counters().chainUpdatesSent;
    link.write("after");
    link.step();
    CHECK_EQ(link.host().counters().chainUpdatesSent, updatesBefore);
    link.sendStaleAck();
    CHECK(link.host().state() == ConnectionState::Attached);
}

// A pane owed a frame inside its rate interval names the interval's end as
// when to be serviced, so an owner that sleeps until then sends it on time.
TEST(theNextServiceIsTheEndOfTheRateInterval)
{
    SyncLink link(0, 24);
    link.write("one");
    link.serviceNow();
    const std::uint64_t sentAt = link.now();
    link.advance(1'000);
    link.write("two");
    const std::optional<std::uint64_t> due = link.host().nextServiceMicroseconds(link.now());
    CHECK(due.has_value());
    CHECK_EQ(due.value_or(0), sentAt + frameInterval);
}

// A checkpoint larger than the window completes: the client acknowledges the
// stream bytes of a transfer that has not finished, so the host's window
// retires them and lets the next chunks leave.
TEST(aCheckpointLargerThanTheWindowArrives)
{
    SyncLink link(0, 120, 300, UbiquittyTest::colourfulScreen(120, 300));
    CHECK_EQ(link.held(), link.newest());
    CHECK(link.host().counters().checkpointsSent == 1);
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A pane unbound while its checkpoint is still crossing takes the rest of the
// transfer with it. The client forgets the transfer when it reads the
// PaneUnbind, which overtakes the chunks on the interactive lane, so a chunk
// that still followed would end the connection and every other pane on it.
TEST(aPaneUnboundMidCheckpointLeavesTheConnectionUp)
{
    SyncLink link(0, 120, 300, UbiquittyTest::colourfulScreen(120, 300));
    // Acks stop, so the resize's checkpoint is cut off by the window after
    // its first chunks.
    link.holdUpstream();
    const std::uint64_t checkpointsBefore = link.host().counters().checkpointsSent;
    link.resize(30, 100);
    link.step();
    REQUIRE(link.host().counters().checkpointsSent == checkpointsBefore + 1);
    REQUIRE(link.host().hasQueuedStreamFrames());
    link.unbind();
    link.releaseUpstream();
    for (int index = 0; index < 16; ++index)
        link.step();
    CHECK(link.client().state() == ConnectionState::Attached);
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(link.client().paneTerminal(link.handle()) == nullptr);
    CHECK(!link.host().hasQueuedStreamFrames());
}

// A checkpoint is bulk, and bulk waits for the window as updates do: with the
// client's acks held, the chunks of a checkpoint several windows long stop at
// the window, and resume when the acks do.
TEST(aCheckpointsChunksWaitForTheWindow)
{
    SyncLink link(0, 120, 300, UbiquittyTest::colourfulScreen(120, 300));
    link.holdUpstream();
    link.resize(30, 100);
    for (int index = 0; index < 8; ++index) {
        link.step();
        CHECK_MSG(link.host().pacer().outstandingBytes() <= link.host().pacer().windowBytes(link.now()),
            ("step " + std::to_string(index) + ": " + std::to_string(link.host().pacer().outstandingBytes()) + " bytes outstanding against a window of " + std::to_string(link.host().pacer().windowBytes(link.now()))).c_str());
    }
    CHECK(link.held() < link.newest());
    link.releaseUpstream();
    for (int index = 0; index < 16; ++index)
        link.step();
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// An update that arrives between a checkpoint's head and its last chunk is
// held, and applied once the checkpoint is: the client ends at the update's
// generation, not the checkpoint's.
TEST(anUpdateThatArrivesDuringACheckpointIsAppliedAfterIt)
{
    SyncLink link(0, 24);
    // The host's stream is captured, so the frames can be handed over with
    // the update between the head and the chunk that completes it.
    std::vector<std::uint8_t> captured;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        captured.insert(captured.end(), stream.begin(), stream.end());
        stream.clear();
    });
    link.resize(30, 100);
    link.step();
    link.write("\x1b[3;1Hafter the checkpoint");
    link.step();
    link.setStreamFilter({});
    FrameDecoder decoder;
    decoder.append(captured);
    std::vector<std::uint8_t> heads;
    std::vector<std::uint8_t> updates;
    std::vector<std::uint8_t> rest;
    while (const std::optional<Frame> frame = decoder.next()) {
        std::vector<std::uint8_t>* into = &rest;
        if (frame->kind == FrameKind::CheckpointHead)
            into = &heads;
        else if (frame->kind == FrameKind::Update)
            into = &updates;
        CHECK(appendFrame(*into, frame->kind, frame->channel, frame->body));
    }
    REQUIRE(!heads.empty());
    REQUIRE(!updates.empty());
    link.deliverToClient(heads);
    link.deliverToClient(updates);
    link.deliverToClient(rest);
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 0 });
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A checkpoint whose head names another state than its body carries is
// refused before anything of it reaches the terminal: the head is what the
// client promised to hold, and a body that is not that state is a peer's
// error, however well it decodes on its own.
TEST(aCheckpointWhoseHeadDisagreesWithItsBodyLeavesTheTerminalUntouched)
{
    SyncLink link(0, 24);
    bool isForged = false;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        if (isForged)
            return;
        FrameDecoder decoder;
        decoder.append(stream);
        std::vector<std::uint8_t> rewritten;
        while (const std::optional<Frame> frame = decoder.next()) {
            std::optional<CheckpointHead> head = frame->kind == FrameKind::CheckpointHead ? decodeCheckpointHead(frame->body) : std::nullopt;
            if (!head) {
                CHECK(appendFrame(rewritten, frame->kind, frame->channel, frame->body));
                continue;
            }
            head->stateDigest.low ^= 1;
            isForged = true;
            std::vector<std::uint8_t> body;
            encode(body, *head);
            CHECK(appendFrame(rewritten, frame->kind, frame->channel, body));
        }
        stream = std::move(rewritten);
    });
    link.writeUnpublished("\x1b[5;1HFORGED");
    link.resize(30, 100);
    link.step();
    REQUIRE(isForged);
    CHECK_EQ(link.clientTerminal().rows, 24);
    CHECK_EQ(link.clientTerminal().columns, 80);
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 1 });

    // The checkpoint the Resync brings is the true one and converges.
    for (int index = 0; index < 4; ++index)
        link.step();
    CHECK_EQ(link.held(), link.newest());
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A connection that closed while a checkpoint was held back by its window
// keeps nothing queued: no Ack will ever open the window again, and the daemon
// reaps a closed connection only once its queue is empty, so a held chunk would
// keep the client, its subscriptions and the checkpoint's body for as long as
// the peer kept its socket open.
TEST(aClosedConnectionKeepsNoTransferQueued)
{
    SyncLink link(0, 120, 300, UbiquittyTest::colourfulScreen(120, 300));
    link.holdUpstream();
    link.resize(30, 100);
    link.step();
    REQUIRE(link.host().hasQueuedStreamFrames());
    const std::array<std::uint8_t, 1> unreadable { 0xff };
    link.sendToHost(FrameKind::Ack, unreadable);
    REQUIRE(link.host().state() == ConnectionState::Closed);
    for (int index = 0; index < 16; ++index)
        link.step();
    CHECK(!link.host().hasQueuedStreamFrames());
}

// While an update waits for the transport no newer one is encoded behind it:
// when the transport next takes the stream, the client receives the waiting
// update and then one to the newest frame, never a replay of every frame
// published while it waited.
TEST(anUpdateWaitingForTheTransportIsNotFollowedByStalerOnes)
{
    SyncLink link(0, 24);
    const std::uint64_t appliedBefore = link.client().counters().updatesApplied;
    for (int frame = 0; frame < 10; ++frame) {
        link.write("frame " + std::to_string(frame) + "\r\n");
        link.serviceWithoutCarrying();
    }
    link.step();
    link.step();
    CHECK_EQ(link.held(), link.newest());
    CHECK_EQ(link.client().counters().updatesApplied - appliedBefore, std::uint64_t { 2 });
    const std::string difference = link.disagreement(link.handle());
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A pane the window refused is not due again until an Ack moves the window:
// an owner that sleeps until the next service would otherwise wake at once,
// be refused again, and spin until the client answers.
TEST(aPaneTheWindowRefusedIsNotDueUntilAnAckArrives)
{
    SyncLink link(0, 24);
    link.holdUpstream();
    for (int index = 0; index < 60 && link.host().counters().windowRefusals == 0; ++index) {
        std::string redraw = "\x1b[H";
        for (int row = 0; row < 24; ++row)
            redraw += "\x1b[" + std::to_string(row + 1) + ";1Hgeneration " + std::to_string(index) + " row " + std::to_string(row) + " " + std::string(60, static_cast<char>('a' + (index + row) % 26));
        link.write(redraw);
        link.step();
    }
    REQUIRE(link.host().counters().windowRefusals > 0);
    link.advance(frameInterval);
    const std::optional<std::uint64_t> refused = link.host().nextServiceMicroseconds(link.now());
    CHECK_MSG(!refused || *refused > link.now(), "a pane the window refused names now as its next service");

    link.releaseUpstream();
    const std::optional<std::uint64_t> acknowledged = link.host().nextServiceMicroseconds(link.now());
    CHECK(acknowledged.has_value());
    CHECK_EQ(acknowledged.value_or(0), link.now());
}

// A stream update waiting for the transport holds every later one back, so
// no pane is due before the transport takes it.
TEST(aPaneBehindAWaitingUpdateIsNotDueUntilTheTransportTakesIt)
{
    SyncLink link(0, 24);
    link.write("one");
    link.serviceWithoutCarrying();
    link.advance(frameInterval);
    link.write("two");
    const std::optional<std::uint64_t> waiting = link.host().nextServiceMicroseconds(link.now());
    CHECK_MSG(!waiting || *waiting > link.now(), "a pane behind a waiting update names now as its next service");

    link.pump();
    const std::optional<std::uint64_t> taken = link.host().nextServiceMicroseconds(link.now());
    CHECK(taken.has_value());
    CHECK_EQ(taken.value_or(0), link.now());
}

// A replica outlives its connection: a second connection over the same
// replica set, bound to the same pane, draws into the same terminal.
TEST(aReplicaOutlivesItsConnection)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::feed(pane.terminal, "kept");
    pane.source->publish(FrameClass::Settled, 2);
    ReplicaSet replicas;
    const ID128 paneID { 9, 10 };
    const Ubiquitty::Terminal* before = nullptr;
    {
        HostConnection host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .principal = "test", .mayDrive = true, .selection = UbiquittyTest::syncDefaultSelection() });
        ClientConnection client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = { 7, 8 }, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts() }, nullptr, nullptr, &replicas);
        client.start();
        const auto pump = [&](std::uint64_t now) {
            host.service(now);
            std::vector<std::uint8_t> stream;
            while (host.popStreamFrame(stream, now)) {
            }
            client.receiveStream(stream, now);
            host.receiveStream(client.takeStreamOutput(), now);
        };
        pump(1);
        pump(1);
        const std::uint32_t handle = host.bindPane(*pane.source);
        for (std::uint64_t now = 2; now < 400 && client.paneGeneration(handle) == 0; ++now)
            pump(now);
        before = client.paneTerminal(handle);
        REQUIRE(before != nullptr);
        CHECK(before == replicas.terminal(paneID));
    }
    // The first connection is gone; the replica is not.
    REQUIRE(replicas.find(paneID) != nullptr);
    CHECK(replicas.find(paneID)->hasCheckpoint);
    HostConnection host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .principal = "test", .mayDrive = true, .selection = UbiquittyTest::syncDefaultSelection() });
    ClientConnection client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = { 7, 8 }, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts() }, nullptr, nullptr, &replicas);
    client.start();
    std::vector<std::uint8_t> stream;
    for (int round = 0; round < 2; ++round) {
        host.service(1);
        stream.clear();
        while (host.popStreamFrame(stream, 1)) {
        }
        client.receiveStream(stream, 1);
        host.receiveStream(client.takeStreamOutput(), 1);
    }
    const std::uint32_t handle = host.bindPane(*pane.source);
    host.service(2);
    stream.clear();
    while (host.popStreamFrame(stream, 2)) {
    }
    client.receiveStream(stream, 2);
    CHECK(client.paneTerminal(handle) == before);
    // Before any frame of the new connection, the picture is the one held.
    CHECK(client.paneDigest(handle).has_value());
}

// Both ends compute one digest from what they hold: over the corpus, a
// client caught up with what the host sent holds the digest the host
// computes from the baseline it sent.
TEST(baselineAndReplicaDigestsAgree)
{
    std::string error;
    const auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    REQUIRE(corpus.has_value());
    std::size_t compared = 0;
    for (const UbiquittyTest::CorpusEntry& entry : *corpus) {
        UbiquittyTest::LoopbackPaneLink link(UbiquittyTest::LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        CHECK_MSG(link.failure().empty(), (entry.name + ": " + link.failure()).c_str());
        const std::optional<std::pair<std::uint64_t, Digest128>> host = link.hostEnd().sentDigest(link.handle());
        REQUIRE(host.has_value());
        if (link.clientEnd().paneGeneration(link.handle()) != host->first)
            continue;
        CHECK_MSG(link.clientEnd().paneDigest(link.handle()) == std::optional<Digest128> { host->second }, (entry.name + ": the two ends' digests differ").c_str());
        ++compared;
    }
    CHECK(compared > 0);
}

UBIQUITTY_TEST_MAIN()

// Every bound pane is a terminal the client keeps, so the pane cap (§7.9) is
// what stops a host from growing a client without bound: the client refuses
// a bind past it, and the host never sends one.
TEST(neitherEndHoldsMorePanesThanTheConnectionAllows)
{
    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    (void)client.takeStreamOutput();
    Welcome welcome;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    welcome.wireVersion = wireVersion;
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Welcome, 0, body));
    client.receiveStream(frame, 1);
    REQUIRE(client.state() == ConnectionState::Attached);
    const auto bindFrame = [](std::uint32_t handle) {
        std::vector<std::uint8_t> bindBody;
        encode(bindBody, PaneBind { .handle = handle, .paneID = { handle, 1 } });
        std::vector<std::uint8_t> bytes;
        CHECK(appendFrame(bytes, FrameKind::PaneBind, 0, bindBody));
        return bytes;
    };
    for (std::uint32_t handle = 1; handle <= maxPanesPerConnection; ++handle)
        client.receiveStream(bindFrame(handle), 1);
    CHECK(client.state() == ConnectionState::Attached);
    client.receiveStream(bindFrame(static_cast<std::uint32_t>(maxPanesPerConnection) + 1), 1);
    CHECK(client.state() == ConnectionState::Closed);
    CHECK(client.byeReason() == ByeReason::Malformed);

    HostIdentity hostIdentity;
    hostIdentity.selection = UbiquittyTest::syncDefaultSelection();
    hostIdentity.attachmentID = { 1, 2 };
    HostConnection host(hostPolicy(0), hostIdentity);
    ClientIdentity clientIdentity;
    clientIdentity.clientInstance = { 3, 4 };
    ClientConnection greeter(clientPolicy, clientIdentity);
    greeter.start();
    host.receiveStream(greeter.takeStreamOutput(), 1);
    REQUIRE(host.state() == ConnectionState::Attached);
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 2);
    std::vector<std::unique_ptr<PaneSource>> sources;
    for (std::size_t index = 0; index <= maxPanesPerConnection; ++index)
        sources.push_back(std::make_unique<PaneSource>(terminal, ID128 { index, 2 }));
    std::uint32_t firstHandle = 0;
    for (std::size_t index = 0; index < maxPanesPerConnection; ++index) {
        const std::uint32_t handle = host.bindPane(*sources[index]);
        CHECK(handle != 0);
        if (!firstHandle)
            firstHandle = handle;
    }
    CHECK_EQ(host.bindPane(*sources.back()), std::uint32_t { 0 });
    // An unbound slot is free again.
    host.unbindPane(firstHandle, 0);
    CHECK(host.bindPane(*sources.back()) != 0);
}

// On QUIC a datagram can overtake the stream: a probe lands ahead of the
// Welcome, an update ahead of its PaneBind. Either is dropped as a lost one
// would be; closing on it would end every QUIC attachment whose first probe
// beat its handshake.
TEST(aDatagramThatOvertakesTheStreamIsDroppedNotFatal)
{
    ClientConnection client(clientPolicy, ClientIdentity {});
    client.start();
    (void)client.takeStreamOutput();
    std::vector<std::uint8_t> body;
    encode(body, Probe { .time = 5 });
    std::vector<std::uint8_t> probe;
    REQUIRE(appendFrame(probe, FrameKind::Probe, 0, body));
    client.receiveDatagram(probe, 1);
    CHECK(client.state() == ConnectionState::Greeting);

    Welcome welcome;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    welcome.wireVersion = wireVersion;
    body.clear();
    encode(body, welcome);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Welcome, 0, body));
    client.receiveStream(frame, 2);
    REQUIRE(client.state() == ConnectionState::Attached);

    // An update for a handle the stream has not bound yet.
    std::vector<std::uint8_t> update;
    std::vector<std::uint8_t> header;
    encode(header, UpdateHeader { .mode = UpdateMode::Chain, .baseGeneration = 0, .generation = 0, .frameClass = FrameClass::Settled, .hostTime = 0 });
    REQUIRE(appendFrame(update, FrameKind::Update, 1, header));
    client.receiveDatagram(update, 3);
    CHECK(client.state() == ConnectionState::Attached);

    // On the stream, the same frames still close the connection.
    client.receiveStream(update, 4);
    CHECK(client.state() == ConnectionState::Closed);
    CHECK(client.byeReason() == ByeReason::UnboundHandle);
}

// A transfer the client gave up on — its pane unbound while its checkpoint
// was arriving — is fenced: a chunk that a reordering carrier delivers late is
// discarded and the connection stays. The host's abort releases the fence,
// and an abort naming a transfer neither announced nor fenced is malformed.
TEST(aChunkAfterAnAbandonIsDiscardedAndTheConnectionStays)
{
    SyncLink link(0, 24);
    link.pump();
    REQUIRE(link.client().state() == ConnectionState::Attached);
    const std::uint32_t handle = link.handle();
    constexpr std::uint32_t transfer = 7'777;
    link.deliverToClient(frameOf(FrameKind::CheckpointHead, handle, CheckpointHead { .generation = 1'000, .transfer = transfer, .frameClass = FrameClass::Structural, .stateDigest = {} }));
    link.deliverToClient(chunkFrame(transfer, 0, 1'000));
    REQUIRE(link.client().state() == ConnectionState::Attached);
    link.deliverToClient(frameOf(FrameKind::PaneUnbind, 0, PaneUnbind { .handle = handle, .reason = 0 }));

    link.deliverToClient(chunkFrame(transfer, 1, 1'000));
    CHECK(link.client().state() == ConnectionState::Attached);
    CHECK_EQ(link.client().transfersHeld(), std::size_t { 0 });

    link.deliverToClient(frameOf(FrameKind::BulkAbort, 0, BulkAbort { .transfer = transfer }));
    CHECK(link.client().state() == ConnectionState::Attached);
    link.deliverToClient(frameOf(FrameKind::BulkAbort, 0, BulkAbort { .transfer = transfer }));
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);
}

// A history head is reassembled under the history cap, not a checkpoint's.
TEST(aHistoryTransferPastFourMebibytesIsRefused)
{
    SyncLink link(0, 24);
    link.pump();
    const std::uint32_t handle = link.handle();
    constexpr std::uint32_t transfer = 5'555;
    link.deliverToClient(frameOf(FrameKind::HistoryHead, handle, HistoryHead { .transfer = transfer, .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending }));
    constexpr std::size_t chunkBytes = 65'000;
    std::uint32_t index = 0;
    for (; index * chunkBytes <= maxHistoryTransferBytes && link.client().state() == ConnectionState::Attached; ++index)
        link.deliverToClient(chunkFrame(transfer, index, chunkBytes));
    CHECK_EQ(index, static_cast<std::uint32_t>(maxHistoryTransferBytes / chunkBytes + 1));
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::TransferRefused);
}
