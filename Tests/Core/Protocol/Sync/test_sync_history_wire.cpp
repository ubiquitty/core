// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history_wire.cpp — the history plane's frames as bytes
// (Docs/sync.md §7.5, §7.9): what each kind's body holds, where each kind
// travels, and the bodies a decoder refuses before it believes them.
//
// - A `HistoryWant` names at most 64 ranges, and the count is refused before a
//   range is read, so a hostile count costs nothing.
// - A range is half-open and non-empty, and a want's ranges are sorted and
//   disjoint, so the host never has to normalise what it was sent.
// - A head announces at least one line, and a reset never names a head below
//   what it cleared.
// - Each kind rides one lane, in one direction, on a pane's channel.
//
// The file links the codecs alone, so the iOS conformance run builds it
// against the phone's archive; what a connection does with the frames is
// test_sync_connection's.

#include "Harness.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Wire.h"

#include <cstdint>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

std::vector<std::uint8_t> wantBody(std::uint32_t epoch, std::uint64_t rangeCount, const std::vector<LineRange>& ranges, std::uint8_t priority)
{
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.u32(epoch);
    writer.varint(rangeCount);
    for (const LineRange& range : ranges) {
        writer.u64(range.from);
        writer.u64(range.to);
    }
    writer.u8(priority);
    return body;
}

std::vector<std::uint8_t> encoded(const auto& message)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    return body;
}

} // namespace

// The count is the cap's to refuse, before a single range is
// read. A body with a count of 65 and no range bytes must be refused as
// over the cap, not as short.
TEST(aWantOverTheMaxRangesIsRefusedBeforeItsRangesAreRead)
{
    const std::vector<std::uint8_t> body = wantBody(1, maxHistoryWantRanges + 1, {}, 0);
    CHECK(!decodeHistoryWant(body));

    std::vector<LineRange> ranges;
    for (std::uint64_t index = 0; index < maxHistoryWantRanges + 1; ++index)
        ranges.push_back({ index * 10, index * 10 + 5 });
    CHECK(!decodeHistoryWant(wantBody(1, ranges.size(), ranges, 0)));

    ranges.pop_back();
    const std::optional<HistoryWant> fullWant = decodeHistoryWant(wantBody(1, ranges.size(), ranges, 0));
    REQUIRE(fullWant.has_value());
    CHECK_EQ(fullWant->ranges.size(), maxHistoryWantRanges);
}

TEST(everyHistoryKindRoundTrips)
{
    const HistoryInlineHeader inlineHeader { .historyEpoch = 3, .firstLine = 0x1'0000'0007 };
    std::vector<std::uint8_t> inlineBody = encoded(inlineHeader);
    const std::size_t headerBytes = inlineBody.size();
    inlineBody.push_back(0xAB); // the lines follow the header
    const std::optional<DecodedHistoryInlineHeader> decodedInline = decodeHistoryInlineHeader(inlineBody);
    REQUIRE(decodedInline.has_value());
    CHECK_EQ(decodedInline->header.historyEpoch, 3u);
    CHECK_EQ(decodedInline->header.firstLine, 0x1'0000'0007u);
    CHECK_EQ(decodedInline->bytes, headerBytes);

    const HistoryHead head { .transfer = 9, .historyEpoch = 2, .firstLine = 4'000'000'000ULL, .count = 256, .order = HistoryOrder::Descending };
    const std::optional<HistoryHead> decodedHead = decodeHistoryHead(encoded(head));
    REQUIRE(decodedHead.has_value());
    CHECK_EQ(decodedHead->transfer, 9u);
    CHECK_EQ(decodedHead->historyEpoch, 2u);
    CHECK_EQ(decodedHead->firstLine, 4'000'000'000ULL);
    CHECK_EQ(decodedHead->count, 256u);
    CHECK(decodedHead->order == HistoryOrder::Descending);

    const HistoryReset reset { .newEpoch = 5, .clearedThrough = 300, .historyHead = 312 };
    const std::optional<HistoryReset> decodedReset = decodeHistoryReset(encoded(reset));
    REQUIRE(decodedReset.has_value());
    CHECK_EQ(decodedReset->newEpoch, 5u);
    CHECK_EQ(decodedReset->clearedThrough, 300u);
    CHECK_EQ(decodedReset->historyHead, 312u);

    const HistoryWant want { .historyEpoch = 5, .ranges = { { 300, 310 }, { 310, 312 }, { 400, 1'000 } }, .priority = HistoryPriority::Search };
    const std::optional<HistoryWant> decodedWant = decodeHistoryWant(encoded(want));
    REQUIRE(decodedWant.has_value());
    CHECK_EQ(decodedWant->historyEpoch, 5u);
    CHECK(decodedWant->ranges == want.ranges);
    CHECK(decodedWant->priority == HistoryPriority::Search);

    // A want with no ranges is how a client withdraws a priority's set.
    const std::optional<HistoryWant> empty = decodeHistoryWant(encoded(HistoryWant { .historyEpoch = 5, .ranges = {}, .priority = HistoryPriority::Backfill }));
    REQUIRE(empty.has_value());
    CHECK(empty->ranges.empty());
}

TEST(aBodyNotConsumedExactlyIsRefused)
{
    std::vector<std::uint8_t> head = encoded(HistoryHead { .transfer = 1, .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending });
    head.push_back(0);
    CHECK(!decodeHistoryHead(head));
    head.pop_back();
    head.pop_back();
    CHECK(!decodeHistoryHead(head));

    std::vector<std::uint8_t> reset = encoded(HistoryReset { .newEpoch = 2, .clearedThrough = 1, .historyHead = 1 });
    reset.push_back(0);
    CHECK(!decodeHistoryReset(reset));

    std::vector<std::uint8_t> want = encoded(HistoryWant { .historyEpoch = 1, .ranges = { { 0, 1 } }, .priority = HistoryPriority::Viewport });
    want.push_back(0);
    CHECK(!decodeHistoryWant(want));

    // The inline header is a prefix; a body too short for it is refused.
    const std::vector<std::uint8_t> inlineBody = encoded(HistoryInlineHeader { .historyEpoch = 1, .firstLine = 1 });
    CHECK(!decodeHistoryInlineHeader(std::span(inlineBody).first(inlineBody.size() - 1)));
}

TEST(anUnknownOrderOrPriorityOrAnEmptyHeadIsRefused)
{
    std::vector<std::uint8_t> head = encoded(HistoryHead { .transfer = 1, .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending });
    head.back() = 2;
    CHECK(!decodeHistoryHead(head));
    CHECK(!decodeHistoryHead(encoded(HistoryHead { .transfer = 1, .historyEpoch = 1, .firstLine = 0, .count = 0, .order = HistoryOrder::Ascending })));

    CHECK(!decodeHistoryWant(wantBody(1, 1, { { 0, 5 } }, 3)));
    CHECK(decodeHistoryWant(wantBody(1, 1, { { 0, 5 } }, 2)));

    // A reset whose head is below the lines it cleared names lines it cannot have.
    CHECK(!decodeHistoryReset(encoded(HistoryReset { .newEpoch = 2, .clearedThrough = 10, .historyHead = 9 })));
}

TEST(anInvertedOrEmptyRangeIsRefused)
{
    CHECK(!decodeHistoryWant(wantBody(1, 1, { { 10, 5 } }, 0)));
    CHECK(!decodeHistoryWant(wantBody(1, 1, { { 5, 5 } }, 0)));
    CHECK(decodeHistoryWant(wantBody(1, 1, { { 5, 6 } }, 0)));
}

TEST(overlappingOrUnsortedRangesAreRefused)
{
    CHECK(!decodeHistoryWant(wantBody(1, 2, { { 0, 10 }, { 9, 20 } }, 0)));
    CHECK(!decodeHistoryWant(wantBody(1, 2, { { 20, 30 }, { 0, 10 } }, 0)));
    CHECK(decodeHistoryWant(wantBody(1, 2, { { 0, 10 }, { 10, 20 } }, 0)));
}

TEST(eachHistoryKindRidesTheInteractiveLaneOnAPaneChannel)
{
    for (const FrameKind kind : { FrameKind::HistoryInline, FrameKind::HistoryHead, FrameKind::HistoryReset, FrameKind::HistoryWant }) {
        CHECK(frameKindOf(static_cast<std::uint8_t>(kind)) == kind);
        CHECK(laneOf(kind) == Lane::Interactive);
        CHECK(!isConnectionKind(kind));
    }
    CHECK_EQ(static_cast<int>(FrameKind::HistoryInline), 40);
    CHECK_EQ(static_cast<int>(FrameKind::HistoryHead), 41);
    CHECK_EQ(static_cast<int>(FrameKind::HistoryReset), 42);
    CHECK_EQ(static_cast<int>(FrameKind::HistoryWant), 43);
    CHECK(directionOf(FrameKind::HistoryInline) == Direction::HostToClient);
    CHECK(directionOf(FrameKind::HistoryHead) == Direction::HostToClient);
    CHECK(directionOf(FrameKind::HistoryReset) == Direction::HostToClient);
    CHECK(directionOf(FrameKind::HistoryWant) == Direction::ClientToHost);
    CHECK(laneOf(FrameKind::BulkAbort) == Lane::Interactive);
}

TEST(aHistoryInlineOnTheConnectionChannelIsRefusedByTheEnvelope)
{
    const std::vector<std::uint8_t> body = encoded(HistoryInlineHeader { .historyEpoch = 1, .firstLine = 0 });
    std::vector<std::uint8_t> frame;
    CHECK(!appendFrame(frame, FrameKind::HistoryInline, 0, body));

    // Hand-built: kind 40, channel 0, the body.
    frame = { static_cast<std::uint8_t>(FrameKind::HistoryInline), 0, static_cast<std::uint8_t>(body.size()) };
    frame.insert(frame.end(), body.begin(), body.end());
    FrameDecoder decoder;
    decoder.append(frame);
    CHECK(!decoder.next());
    CHECK(decoder.error() == EnvelopeError::Malformed);
}

UBIQUITTY_TEST_MAIN()
