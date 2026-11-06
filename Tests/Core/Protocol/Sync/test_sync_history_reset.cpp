// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history_reset.cpp — a destructive clear purges retained history
// (Docs/sync.md §6.6, §7.5).
//
// - A destructive clear on the host reaches the client as a `HistoryReset`
//   ahead of anything else of the pane, and the client purges its cache, the
//   copies its purge sink owns and its replica's scrollback before it applies
//   anything after.
// - Every history transfer of the old epoch is aborted, and the epoch fence
//   discards what of one is still arriving, however a carrier reorders it, so
//   a late chunk never repopulates the purged cache.
// - A forged line of the old epoch is discarded, and a checkpoint taken
//   before the clear never rewinds the epoch.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

constexpr std::string_view sentinel = "SENTINEL-7781";

std::string lines(int first, int count, std::size_t width = 0, std::string_view prefix = "line ")
{
    std::string bytes;
    for (int index = first; index < first + count; ++index) {
        std::string line = std::string(prefix) + std::to_string(index);
        line.resize(std::max(line.size(), width), '.');
        bytes += line + "\r\n";
    }
    return bytes;
}

Ubiquitty::Screen::HistoryAddress hostAddress(const SyncLink& link)
{
    return Ubiquitty::Screen::historyAddress(link.hostTerminal());
}

const HistoryCache& cacheOf(const SyncLink& link)
{
    return *link.client().historyCache(link.handle());
}

std::string textOf(const DecodedLine& line)
{
    std::string text;
    for (const Ubiquitty::Cell& cell : line.cells)
        text.push_back(cell.codePoint >= 0x20 && cell.codePoint < 0x7F ? static_cast<char>(cell.codePoint) : '?');
    return text;
}

// Whether any line the cache holds, or any row of the replica's scrollback,
// says `needle`.
bool clientHolds(const SyncLink& link, std::string_view needle)
{
    const HistoryCache& cache = cacheOf(link);
    for (const LineRange& range : cache.cachedRanges()) {
        for (std::uint64_t number = range.from; number < range.to; ++number) {
            if (textOf(*cache.line(number)).find(needle) != std::string::npos)
                return true;
        }
    }
    const Ubiquitty::Terminal& replica = link.clientTerminal();
    for (int row = 0; row < Ubiquitty::Screen::scrollbackCount(&replica); ++row) {
        if (Ubiquitty::Screen::absoluteRowText(replica, row, replica.columns).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

template<typename Message>
std::vector<std::uint8_t> frameOf(FrameKind kind, std::uint32_t channel, const Message& message, std::span<const std::uint8_t> tail = {})
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    body.insert(body.end(), tail.begin(), tail.end());
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, kind, channel, body));
    return frame;
}

// A history body of lines a host never had, for a forger.
std::vector<std::uint8_t> forgedBody(std::uint64_t& count)
{
    auto scratch = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*scratch, 4, 60);
    UbiquittyTest::feed(*scratch, lines(0, 20, 0, "FORGED "));
    HistoryPublisher publisher;
    (void)publisher.capture(*scratch);
    const Ubiquitty::Screen::HistoryAddress address = publisher.address();
    std::vector<std::uint8_t> body;
    count = publisher.writeBody(*scratch, { address.oldest, address.head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body);
    return body;
}

// A link whose window lets one chunk of a transfer leave and holds the next
// until the client acknowledges it.
std::unique_ptr<SyncLink> oneChunkLink()
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.pacing.windowFloorBytes = 100 * 1024;
    auto link = std::make_unique<SyncLink>(0, UbiquittyTest::syncDefaultFacts(), policy);
    (void)link->addPane(24, 80, "$ ");
    return link;
}

// Drives a flood whose appends travel as transfers of more than one chunk into
// the lanes, with the client neither acknowledging nor reading, so chunks of
// the old epoch stay queued on the host and held on the link.
void floodIntoTheLanes(SyncLink& link)
{
    link.stepUntilQuiet();
    link.holdUpstream();
    link.holdDownstream();
    link.write(lines(0, 600, 400));
    for (int step = 0; step < 4; ++step)
        link.step();
}

} // namespace

// The §10 negative: a reset that lands between two chunks of an
// old-epoch transfer leaves nothing of the transfer in the cache, on a stream
// and on a carrier that delivers the second chunk after the reset.
TEST(aResetBetweenTwoChunksOfAnOldEpochTransferLeavesNothingOfIt)
{
    for (const bool isReordered : { false, true }) {
        const std::unique_ptr<SyncLink> owner = oneChunkLink();
        SyncLink& link = *owner;
        floodIntoTheLanes(link);
        std::optional<HistoryHead> first;
        REQUIRE(link.releaseDownstreamThrough([&](FrameKind kind, std::span<const std::uint8_t> body) {
            if (kind == FrameKind::HistoryHead && !first)
                first = decodeHistoryHead(body);
            return kind == FrameKind::BulkChunk && first && decodeBulkChunk(body)->transfer == first->transfer;
        }));
        REQUIRE(first.has_value());
        const std::uint64_t abortsBefore = link.host().counters().bulkAborts;

        link.write("\x1b[3J");
        link.step();
        CHECK(link.host().counters().bulkAborts > abortsBefore);
        link.releaseUpstream();
        if (isReordered)
            link.releaseDownstreamReordered([](FrameKind kind, std::span<const std::uint8_t>) { return kind == FrameKind::BulkChunk; });
        else
            link.releaseDownstream();
        link.stepUntilQuiet();

        for (std::uint64_t number = first->firstLine; number < first->firstLine + first->count; ++number)
            CHECK(!cacheOf(link).line(number));
        CHECK(link.client().state() == ConnectionState::Attached);
        CHECK_EQ(cacheOf(link).key().epoch, hostAddress(link).epoch);
        CHECK_STR_EQ(link.disagreement(link.handle()), "");
    }
}

// The cache half of the §10 negative: after a clear, no cleared line is in the
// cache or in the replica's scrollback.
TEST(theCacheAndReplicaHoldNoClearedLineAfterAClear)
{
    SyncLink link(0, 24);
    link.write(lines(0, 50, 0, sentinel));
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    REQUIRE(clientHolds(link, sentinel));
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    CHECK(!clientHolds(link, sentinel));
    CHECK_EQ(link.client().counters().historyResetsApplied, std::uint64_t { 1 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A line of the old epoch that arrives after the reset, however it got
// there, is discarded and the connection stays.
TEST(aForgedOldEpochInlineAfterAResetIsDiscarded)
{
    SyncLink link(0, 24);
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    const std::uint32_t oldEpoch = hostAddress(link).epoch;
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    const std::size_t held = cacheOf(link).lineCount();
    std::uint64_t count = 0;
    const std::vector<std::uint8_t> body = forgedBody(count);
    REQUIRE(count > 0);
    link.deliverToClient(frameOf(FrameKind::HistoryInline, link.handle(), HistoryInlineHeader { .historyEpoch = oldEpoch, .firstLine = cacheOf(link).head() }, body));
    CHECK(link.client().state() == ConnectionState::Attached);
    CHECK_EQ(cacheOf(link).lineCount(), held);
    CHECK(!clientHolds(link, "FORGED"));
}

TEST(aChunkOfAnOldEpochTransferNeverRepopulatesTheCache)
{
    SyncLink link(0, 24);
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    const std::uint32_t oldEpoch = hostAddress(link).epoch;
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    std::uint64_t count = 0;
    const std::vector<std::uint8_t> body = forgedBody(count);

    LaneScheduler scheduler(LanePolicy { .bulkSharePermille = 1000 });
    const HistoryHead head { .transfer = 9'000, .historyEpoch = oldEpoch, .firstLine = cacheOf(link).head(), .count = static_cast<std::uint16_t>(count), .order = HistoryOrder::Ascending };
    std::vector<std::uint8_t> headBody;
    encode(headBody, head);
    // The scheduler numbers its own transfers; the chunks are renumbered to
    // the head's transfer below.
    (void)scheduler.enqueueTransfer(FrameKind::HistoryHead, link.handle(), headBody, body);
    std::vector<std::uint8_t> stream;
    while (scheduler.popFrame(stream, BulkPacing::Admitted)) {
    }
    FrameDecoder decoder;
    decoder.append(stream);
    std::vector<std::uint8_t> renumbered;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind != FrameKind::BulkChunk) {
            CHECK(appendFrame(renumbered, frame->kind, frame->channel, frame->body));
            continue;
        }
        BulkChunk chunk = *decodeBulkChunk(frame->body);
        chunk.transfer = head.transfer;
        std::vector<std::uint8_t> chunkBody;
        encode(chunkBody, chunk);
        CHECK(appendFrame(renumbered, FrameKind::BulkChunk, 0, chunkBody));
    }
    const std::size_t held = cacheOf(link).lineCount();
    link.deliverToClient(renumbered);
    CHECK(link.client().state() == ConnectionState::Attached);
    CHECK_EQ(cacheOf(link).lineCount(), held);
    CHECK_EQ(link.client().transfersHeld(), std::size_t { 0 });
    CHECK(!clientHolds(link, "FORGED"));
}

// The purge reaches the copies the connection does not own before anything
// after the reset applies: the update that came with the clear included.
TEST(thePurgeSinkRunsBeforeAnythingElseApplies)
{
    SyncLink link(0, 24);
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    const HistoryKey before = cacheOf(link).key();
    const std::size_t linesBefore = cacheOf(link).lineCount();
    REQUIRE(linesBefore > 0);
    const std::uint64_t updatesBefore = link.client().counters().updatesApplied;
    std::vector<std::pair<HistoryKey, std::uint64_t>> purges;
    std::uint64_t updatesAtPurge = 0;
    std::size_t linesAtPurge = 0;
    link.onPurge([&](const HistoryKey& retired, std::uint64_t clearedThrough) {
        purges.emplace_back(retired, clearedThrough);
        updatesAtPurge = link.client().counters().updatesApplied;
        linesAtPurge = cacheOf(link).lineCount();
        CHECK(cacheOf(link).key() == before);
    });
    link.write("\x1b[3Jafter the clear\r\n");
    link.stepUntilQuiet();
    REQUIRE(purges.size() == 1);
    CHECK(purges[0].first == before);
    CHECK_EQ(purges[0].second, hostAddress(link).clearedThrough);
    CHECK_EQ(updatesAtPurge, updatesBefore);
    // Retirement may refuse: the committed cache is still intact while the
    // sink decides, and only its successful return permits the reset.
    CHECK_EQ(linesAtPurge, linesBefore);
    CHECK(cacheOf(link).key().epoch > before.epoch);
    CHECK(link.client().counters().updatesApplied > updatesBefore);
}

// What a resume record would be built from holds nothing of the old epoch:
// the cache's ranges, and a checkpoint of the replica.
TEST(theResumeInputsHoldNoOldEpochLine)
{
    SyncLink link(0, 24);
    link.write(lines(0, 50, 0, sentinel));
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    const std::uint64_t clearedThrough = hostAddress(link).clearedThrough;
    for (const LineRange& range : cacheOf(link).cachedRanges())
        CHECK(range.from >= clearedThrough);

    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(link.clientTerminal()) && publisher.checkpoint(link.clientTerminal(), 1, body, baseline));
    CHECK(std::ranges::search(body, sentinel).empty());
}

// A checkpoint taken before the clear, delivered after the client applied the
// reset, shows its screen and leaves the epoch where the reset put it.
TEST(aCheckpointTakenBeforeTheClearDoesNotRewindTheEpoch)
{
    SyncLink link(0, 24);
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> stale;
    REQUIRE(publisher.capture(link.hostTerminal()) && publisher.checkpoint(link.hostTerminal(), 1'000'000, stale, baseline));
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    const std::uint32_t epoch = cacheOf(link).key().epoch;
    REQUIRE(epoch == hostAddress(link).epoch);

    LaneScheduler scheduler(LanePolicy { .bulkSharePermille = 1000 });
    std::array<std::uint8_t, 16> digestBytes {};
    std::copy(stale.end() - 16, stale.end(), digestBytes.begin());
    Digest128 digest;
    for (int index = 0; index < 8; ++index) {
        digest.low |= std::uint64_t { digestBytes[static_cast<std::size_t>(index)] } << (8 * index);
        digest.high |= std::uint64_t { digestBytes[static_cast<std::size_t>(8 + index)] } << (8 * index);
    }
    std::vector<std::uint8_t> headBody;
    encode(headBody, CheckpointHead { .generation = 1'000'000, .transfer = scheduler.nextTransferID(), .frameClass = FrameClass::Structural, .stateDigest = digest });
    (void)scheduler.enqueueTransfer(FrameKind::CheckpointHead, link.handle(), headBody, stale);
    std::vector<std::uint8_t> stream;
    while (scheduler.popFrame(stream, BulkPacing::Admitted)) {
    }
    const std::uint64_t appliedBefore = link.client().counters().checkpointsApplied;
    link.deliverToClient(stream);
    CHECK_EQ(link.client().counters().checkpointsApplied, appliedBefore + 1);
    CHECK_EQ(cacheOf(link).key().epoch, epoch);
    CHECK_EQ(link.client().counters().historyResetsApplied, std::uint64_t { 1 });
}

TEST(aHardResetPurgesLikeEraseSavedLines)
{
    SyncLink link(0, 24);
    link.write(lines(0, 50, 0, sentinel));
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    REQUIRE(clientHolds(link, sentinel));
    link.write("\x1b"
               "c");
    link.write(lines(0, 40));
    link.stepUntilQuiet();
    CHECK(!clientHolds(link, sentinel));
    CHECK_EQ(link.client().counters().historyResetsApplied, std::uint64_t { 1 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A partial clear renumbers the lines it keeps above the old head, so they
// travel again under the new epoch, and the client holds them as the host does.
TEST(aClearToAPreviousMarkResendsTheSurvivorsUnderTheNewEpoch)
{
    SyncLink link(0, 12);
    std::string session;
    for (int command = 0; command < 30; ++command)
        session += "\033]133;D;0\007\033]133;A\007$ echo " + std::to_string(command) + "\r\n\033]133;C\007" + std::to_string(command) + "\r\n";
    session += "\033]133;D;0\007\033]133;A\007$ ";
    link.write(session);
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress before = hostAddress(link);
    link.edit([](Ubiquitty::Terminal& terminal) { (void)Ubiquitty::Screen::runClearCommand(Ubiquitty::Screen::ClearCommand::ToPreviousMark, &terminal, nullptr, {}); });
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress after = hostAddress(link);
    REQUIRE(after.epoch == before.epoch + 1);
    REQUIRE(after.head > after.clearedThrough);
    CHECK_EQ(cacheOf(link).key().epoch, after.epoch);
    CHECK(cacheOf(link).line(after.clearedThrough) != nullptr);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// Every old-epoch transfer the client heard of is aborted with a BulkAbort;
// one whose head never left is dropped without a word.
TEST(aResetAbortsEveryOldEpochTransfer)
{
    const std::unique_ptr<SyncLink> owner = oneChunkLink();
    SyncLink& link = *owner;
    floodIntoTheLanes(link);
    const std::vector<HostConnection::QueuedHistoryTransfer> queued = link.host().queuedHistoryTransfers(link.handle());
    REQUIRE(!queued.empty());
    const auto announced = static_cast<std::uint64_t>(std::ranges::count_if(queued, [](const auto& transfer) { return transfer.isAnnounced; }));
    const std::uint64_t abortsBefore = link.host().counters().bulkAborts;
    link.write("\x1b[3J");
    link.step();
    CHECK_EQ(link.host().counters().bulkAborts - abortsBefore, announced);
    CHECK(link.host().queuedHistoryTransfers(link.handle()).empty());
    link.releaseUpstream();
    link.releaseDownstream();
    link.stepUntilQuiet();
    CHECK(link.client().state() == ConnectionState::Attached);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// Repeated clears at an empty prompt cost nothing: an epoch in which no line
// was numbered opens no new one, so no reset is sent.
TEST(tenClearsOnAnEmptyHistorySendNoReset)
{
    SyncLink link(0, 24);
    link.stepUntilQuiet();
    for (int clear = 0; clear < 10; ++clear) {
        link.write("\x1b[3J");
        link.step();
    }
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyResetsSent, std::uint64_t { 0 });

    link.write(lines(0, 30));
    link.stepUntilQuiet();
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyResetsSent, std::uint64_t { 1 });
    for (int clear = 0; clear < 10; ++clear) {
        link.write("\x1b[3J");
        link.step();
    }
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyResetsSent, std::uint64_t { 1 });
}

UBIQUITTY_TEST_MAIN()
