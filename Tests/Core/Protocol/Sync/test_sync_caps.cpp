// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_caps.cpp — what bounds the memory a connection holds for its
// client (Docs/sync.md §6.5, §7.9).
//
// The properties held:
//
// - Bulk is materialised lazily: however many panes owe a checkpoint, one
//   checkpoint body is encoded and held per connection at a time.
// - Replies beyond the outstanding-call cap, or past the queued-bulk cap, are
//   refused and the connection stays; `Welcome.limits` says what the caps are.
// - An `Ack` naming more panes than a connection may hold is refused.
// - A pen dictionary that grows past its cap rolls the epoch, and a client
//   refuses an update that defines past it.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// A canonical JSON-RPC reply carrying `bytes` of text.
///
/// - Parameter bytes: The text's length.
/// - Returns: The whole Sync frame.
std::string reply(std::size_t bytes, int callID)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(callID) + R"(,"result":")" + std::string(bytes, 'r') + R"("})";
}

/// The policy whose caps the tests read.
const HostPolicy policy = UbiquittyTest::syncHostPolicy(0);

} // namespace

// Thirty-two panes, each with a large screen, are bound at once while the
// client acknowledges nothing. The connection encodes the next checkpoint only
// once the one before has left the lanes, so it never holds more than one body;
// once acknowledgements flow every pane converges.
TEST(bindingManyPanesMaterialisesOneCheckpointAtATime)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    link.holdUpstream();
    for (int pane = 0; pane < 32; ++pane)
        (void)link.bindPane(60, 200, UbiquittyTest::colourfulScreen(60, 200));
    std::size_t largest = 0;
    for (int step = 0; step < 64; ++step) {
        link.step();
        largest = std::max(largest, link.host().queuedBulkBytes());
    }
    const std::size_t oneCheckpoint = 60 * 200 * 4;
    CHECK_MSG(largest <= 2 * oneCheckpoint,
        ("the lanes held " + std::to_string(largest) + " bulk bytes; one checkpoint is about " + std::to_string(oneCheckpoint)).c_str());
    link.releaseUpstream();
    for (int step = 0; step < 4'000 && link.client().counters().checkpointsApplied < 32; ++step)
        link.step();
    CHECK_EQ(link.client().counters().checkpointsApplied, std::uint64_t { 32 });
    for (const std::uint32_t handle : link.handles()) {
        CHECK_STR_EQ(link.disagreement(handle), "");
    }
}

// A transport that takes no output retains at most the outstanding-call
// limit of canonical replies; past it the owner refuses further work without
// closing the connection. A reply past the byte cap is refused at any count.
TEST(repliesPastTheLimitsAreRefusedAndTheConnectionStays)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.holdDownstream();
    constexpr std::size_t answerBytes = 4'000;
    std::uint64_t admitted = 0;
    std::uint64_t refused = 0;
    for (int call = 0; call < 1'000; ++call) {
        const std::string answer = reply(answerBytes, call + 1);
        if (!link.host().admitsCall(answer.size())) {
            ++refused;
            continue;
        }
        REQUIRE(link.client().call(R"({"jsonrpc":"2.0","id":)" + std::to_string(call + 1) + R"(,"method":"pane.capture","params":{"pane":1}})"));
        link.host().receiveStream(link.client().takeStreamOutput(), link.now());
        const auto calls = link.host().takeCalls();
        REQUIRE(calls.size() == 1 && calls.front().admission == CallAdmission::Admitted);
        REQUIRE(link.host().answerCall(calls.front().callIndex, answer) == CallAnswer::Sent);
        ++admitted;
    }
    CHECK_EQ(admitted, policy.limits.maxOutstandingCalls);
    CHECK_EQ(admitted + refused, std::uint64_t { 1'000 });
    CHECK_MSG(link.host().queuedBytes() <= policy.limits.maxOutstandingCalls * (answerBytes + 64),
        ("the host holds " + std::to_string(link.host().queuedBytes()) + " bytes for a client that reads nothing").c_str());
    CHECK(link.host().state() == ConnectionState::Attached);

    // Once the client reads, the replies leave and calls are admitted again.
    link.releaseDownstream();
    for (int step = 0; step < 256; ++step)
        link.step();
    CHECK(link.host().admitsCall(answerBytes));
    CHECK_EQ(link.client().takeResults().size(), std::size_t { admitted });

    // A reply larger than the bulk limit is refused even with nothing held.
    CHECK(!link.host().admitsCall(policy.limits.maxQueuedBulkBytes + 1));
}

// The caps a client can hit are advertised in its Welcome.
TEST(welcomeAdvertisesTheLimits)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    REQUIRE(link.client().welcome().has_value());
    const Limits& limits = link.client().welcome()->limits;
    CHECK_EQ(limits.maxOutstandingCalls, policy.limits.maxOutstandingCalls);
    CHECK_EQ(limits.maxQueuedBulkBytes, policy.limits.maxQueuedBulkBytes);
    CHECK_EQ(limits.maxPendingRanges, policy.limits.maxPendingRanges);

    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.selection = link.client().welcome()->selection;
    welcome.limits = { .maxOutstandingCalls = 7, .maxQueuedBulkBytes = 1'234'567, .maxPendingRanges = 9 };
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    const std::optional<Welcome> decoded = decodeWelcome(body);
    REQUIRE(decoded.has_value());
    CHECK_EQ(decoded->limits.maxOutstandingCalls, std::uint16_t { 7 });
    CHECK_EQ(decoded->limits.maxQueuedBulkBytes, std::uint32_t { 1'234'567 });
    CHECK_EQ(decoded->limits.maxPendingRanges, std::uint16_t { 9 });
}

// An Ack names each pane it acknowledges once, and a connection holds at most
// the pane cap, so a longer list is refused before any element is read.
TEST(anAckNamingMorePanesThanAConnectionHoldsIsRefused)
{
    Ack ack;
    for (std::uint32_t handle = 1; handle <= maxPanesPerConnection; ++handle)
        ack.panes.push_back({ .handle = handle, .generation = 1 });
    std::vector<std::uint8_t> body;
    encode(body, ack);
    CHECK(decodeAck(body).has_value());
    ack.panes.push_back({ .handle = static_cast<std::uint32_t>(maxPanesPerConnection) + 1, .generation = 1 });
    body.clear();
    encode(body, ack);
    CHECK(!decodeAck(body).has_value());
}

// A pane that keeps drawing in new colours defines a pen for each. Once its
// updates have defined a cap's worth since the checkpoint the epoch began
// with, the epoch rolls and the client is sent a checkpoint of the new one.
TEST(aPenDictionaryPastItsLimitRollsTheEpoch)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(24, 80));
    const std::uint32_t pane = link.addPane(24, 80, "");
    const std::uint64_t checkpointsBefore = link.host().counters().checkpointsSent;
    int colour = 0;
    for (int frame = 0; frame < 40; ++frame) {
        std::string bytes = "\x1b[H";
        for (int cell = 0; cell < 24 * 80 - 1; ++cell, ++colour)
            bytes += "\x1b[38;2;" + std::to_string(colour & 0xFF) + ";" + std::to_string((colour >> 8) & 0xFF) + ";" + std::to_string((colour >> 16) & 0xFF) + "mx";
        link.write(pane, bytes);
        link.step();
        link.step();
    }
    CHECK_MSG(colour > static_cast<int>(maxPensPerEpoch), "premise: the pane defined fewer pens than the cap");
    CHECK_MSG(link.host().counters().checkpointsSent > checkpointsBefore, "the epoch never rolled");
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// A client holds the host to the per-epoch limit: an update that defines a pen
// past the pens its checkpoint brought and a limit's worth more is refused
// before anything of it applies.
TEST(anUpdateDefiningPastThePenLimitIsRefused)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane(24, 80, "");
    std::vector<std::uint8_t> captured;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        captured.insert(captured.end(), stream.begin(), stream.end());
        stream.clear();
    });
    // The client asks for a checkpoint; the update after it chains from it.
    link.client().requestCheckpoint(pane);
    link.step();
    link.step();
    link.write(pane, "\x1b[38;2;1;2;3mnew pen");
    link.step();
    link.setStreamFilter({});
    FrameDecoder decoder;
    decoder.append(captured);
    std::vector<std::uint8_t> checkpointBody;
    std::vector<std::uint8_t> updateBody;
    BulkReassembler reassembler;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind == FrameKind::CheckpointHead) {
            const std::optional<CheckpointHead> head = decodeCheckpointHead(frame->body);
            REQUIRE(head.has_value());
            CHECK(reassembler.expect(head->transfer, 8 << 20));
        } else if (frame->kind == FrameKind::BulkChunk) {
            const std::optional<BulkChunk> chunk = decodeBulkChunk(frame->body);
            REQUIRE(chunk.has_value());
            if (reassembler.accept(*chunk) == ChunkOutcome::Complete)
                checkpointBody = reassembler.take(chunk->transfer);
        } else if (frame->kind == FrameKind::Update) {
            updateBody.assign(frame->body.begin(), frame->body.end());
        }
    }
    REQUIRE(!checkpointBody.empty());
    REQUIRE(!updateBody.empty());

    const auto applyHoldingMaxPens = [&](bool isAtMaxPens) -> ApplyOutcome {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 24, 80);
        ScreenReplica replica;
        if (applyCheckpoint(terminal, checkpointBody, replica) != ApplyOutcome::Applied)
            return ApplyOutcome::BaseMissing;
        if (isAtMaxPens)
            replica.maxPens = replica.pens().size();
        const std::optional<DecodedUpdateHeader> header = decodeUpdateHeader(updateBody);
        if (!header)
            return ApplyOutcome::Malformed;
        return applyRowUpdate(terminal, std::span<const std::uint8_t>(updateBody).subspan(header->bytes), replica);
    };
    CHECK(applyHoldingMaxPens(false) == ApplyOutcome::Applied);
    CHECK(applyHoldingMaxPens(true) == ApplyOutcome::Malformed);
}

UBIQUITTY_TEST_MAIN()
