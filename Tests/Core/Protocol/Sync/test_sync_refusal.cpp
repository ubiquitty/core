// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_refusal.cpp — a checkpoint the client cannot take is not a loop.
//
// A client that refuses a checkpoint asks for another, and a host asked for a
// checkpoint sends one. When the client refuses every body the host can make,
// the two would trade them for as long as the connection lasts. The properties
// held:
//
// - The client asks again at once after one refusal, then after a wait that
//   doubles, and after `maxCheckpointRefusals` in a row it stops, reports the
//   pane failed with the reason, and takes no screen for it.
// - The host sends a refused state once more, and not after a second
//   refusal: the body is withheld until the pane changes, and the change is
//   sent and taken.
//
// The refusing client is a real one behind a link that corrupts the state
// digest of every `CheckpointHead`, or of the first, so the body is never the
// state its head names.

#include "CheckpointConnectionFixture.h"
#include "Harness.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// Corrupts the state digest of checkpoint heads in a carry.
///
/// - Parameters:
///   - stream: The host's stream bytes, whole frames.
///   - budget: How many more heads to corrupt; decremented.
void corruptHeads(std::vector<std::uint8_t>& stream, int& budget)
{
    FrameDecoder decoder;
    decoder.append(stream);
    std::vector<std::uint8_t> rewritten;
    while (const std::optional<Frame> frame = decoder.next()) {
        std::vector<std::uint8_t> body(frame->body.begin(), frame->body.end());
        if (frame->kind == FrameKind::CheckpointHead && budget > 0 && !body.empty()) {
            body.back() ^= 0x5A;
            --budget;
        }
        CHECK(appendFrame(rewritten, frame->kind, frame->channel, body));
    }
    stream = std::move(rewritten);
}

} // namespace

// Every head corrupted, and the pane changing every frame so the host always
// has a new state to send: the client asks at most `maxCheckpointRefusals`
// times, at growing intervals, then reports the pane failed and takes no
// screen for it.
TEST(aClientThatRefusesEveryCheckpointStopsAndReportsThePaneFailed)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.bindPane(24, 80, "$ ");
    int budget = 1'000'000;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) { corruptHeads(stream, budget); });
    std::vector<std::uint64_t> resyncTimes;
    std::uint64_t resyncsSeen = 0;
    for (int frame = 0; frame < 60 * 60; ++frame) {
        link.write(pane, "\x1b[2;1Hframe " + std::to_string(frame));
        link.step();
        if (link.client().counters().resyncsSent > resyncsSeen) {
            resyncsSeen = link.client().counters().resyncsSent;
            resyncTimes.push_back(link.now());
        }
    }
    const ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    const std::uint64_t refused = link.host().counters().checkpointsRefused;
    CHECK_MSG(link.client().counters().resyncsSent < policy.maxCheckpointRefusals, ("the client asked " + std::to_string(link.client().counters().resyncsSent) + " times").c_str());
    CHECK_MSG(refused < policy.maxCheckpointRefusals, ("the host was told of " + std::to_string(refused) + " refusals").c_str());
    REQUIRE(link.client().paneFailure(pane).has_value());
    CHECK(link.client().paneFailure(pane)->find("didn’t match what the host said it sent") != std::string::npos);
    CHECK_EQ(link.client().counters().panesFailed, std::uint64_t { 1 });
    // The asks after the first wait, each longer than the one before.
    for (std::size_t index = 2; index < resyncTimes.size(); ++index)
        CHECK(resyncTimes[index] - resyncTimes[index - 1] > resyncTimes[index - 1] - resyncTimes[index - 2]);
    // A failed pane takes no screen: the host holds it RecordsOnly, and a
    // frontend asking for it Live again is not heeded.
    CHECK(link.host().paneView(pane) == ViewState::RecordsOnly);
    link.client().setView(pane, ViewState::Live);
    const std::uint64_t checkpointsAfter = link.host().counters().checkpointsSent;
    for (int frame = 0; frame < 600; ++frame) {
        link.write(pane, "\x1b[3;1Hlater " + std::to_string(frame));
        link.step();
    }
    CHECK_EQ(link.host().counters().checkpointsSent, checkpointsAfter);
}

// The first two heads corrupted and the pane still. One refusal may be a body
// the path spoiled, so the state is sent once more; refused twice, the host
// does not send it again however often the client asks. Once the pane
// changes, the new state is sent and taken.
TEST(aHostDoesNotResendAStateTheClientRefused)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.bindPane(24, 80, "$ ");
    int budget = 2;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) { corruptHeads(stream, budget); });
    for (int frame = 0; frame < 60 * 30; ++frame)
        link.step();
    CHECK_EQ(link.host().counters().checkpointsRefused, std::uint64_t { 2 });
    CHECK_MSG(link.host().counters().checkpointsSent == 2, ("the host sent " + std::to_string(link.host().counters().checkpointsSent) + " checkpoints of one state").c_str());
    CHECK(link.host().counters().refusedCheckpointsWithheld >= 1);
    CHECK(link.held(pane) == 0);

    link.write(pane, "changed");
    for (int frame = 0; frame < 120 && link.held(pane) != link.newest(pane); ++frame)
        link.step();
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK(!link.client().paneFailure(pane).has_value());
    CHECK(UbiquittyTest::firstDisagreement(link.hostTerminal(pane), link.clientTerminal(pane)).empty());
}

namespace {

/// The pane-failure reasons a delegate hears.
class FailureRecorder final : public ClientPaneDelegate {
public:
    void paneFailed(std::uint32_t, ID128, std::string_view reason) override { failures.emplace_back(reason); }
    void paneUnbound(std::uint32_t, ID128) override { }
    void paneBound(std::uint32_t, ID128) override { }

    std::vector<std::string> failures;
};

} // namespace

// The handshake admits only a host of this build, so a checkpoint body that
// does not decode after it was damaged on the way: it fails the pane with its
// own sentence, which is neither the different-builds refusal nor a log line.
TEST(aCheckpointThatDoesNotDecodeAfterAMatchingHandshakeIsReportedAsDamage)
{
    using namespace UbiquittyTest::CheckpointConnection;
    ClientPolicy policy { .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 20, .minAckIntervalMicroseconds = 4'000 };
    policy.maxCheckpointRefusals = 1;
    FailureRecorder recorder;
    ClientConnection client(policy, identityFixture(), &recorder, nullptr, nullptr);
    client.start();
    (void)client.takeStreamOutput();
    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    client.receiveStream(messageFrame(FrameKind::Welcome, 0, welcome), 1);
    REQUIRE(client.state() == ConnectionState::Attached);
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);

    // A real body cut short before the digest it ends with: the head still
    // names it, and the transfer's SHA-256 is of the bytes as they arrive.
    auto source = makeTerminal(3, 9);
    UbiquittyTest::feed(*source, "small");
    std::vector<std::uint8_t> body = checkpointOf(*source);
    WireReader digestReader { std::span<const std::uint8_t>(body).last(16) };
    const Digest128 digest { .low = digestReader.u64(), .high = digestReader.u64() };
    body.erase(body.end() - 17);
    client.receiveStream(messageFrame(FrameKind::CheckpointHead, 1, CheckpointHead { .generation = 1, .transfer = 900, .frameClass = FrameClass::Structural, .stateDigest = digest }), 3);
    BulkChunk chunk { .transfer = 900, .index = 0, .isLast = true, .bytes = body };
    Ubiquitty::Crypto::sha256(body, chunk.sha256);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, chunk), 3);

    CHECK(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.counters().checkpointsApplied, 0u);
    REQUIRE(recorder.failures.size() == 1u);
    const std::string& reason = recorder.failures.front();
    CHECK_MSG(reason.find("This pane’s contents") == 0, reason.c_str());
    CHECK(reason.find("Reconnect") != std::string::npos);
    CHECK(reason != versionMismatchNotice());
    CHECK(reason.find("body") == std::string::npos);
    CHECK(reason.find("decode") == std::string::npos);
}

UBIQUITTY_TEST_MAIN()
