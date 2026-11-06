// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history_wants.cpp — history a client asks for (Docs/sync.md
// §6.6, §7.9): a want replaces the set at its priority, latest wins, under a
// cap on the ranges every priority holds together.
//
// - A thousand wants with no service between them leave one transfer, and
//   the cache ends holding the last want's lines.
// - A want that no longer covers a transfer in flight aborts it.
// - A want is clipped to what the host can send in the current epoch, and
//   one of another epoch is ignored.
// - A want over the cap is refused whole and silently; the sets stand and
//   the connection stays.
// - `Viewport` and `Search` wants are served while backfill runs.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/HistoryWants.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Terminal.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

std::string lines(int first, int count, std::size_t width = 0)
{
    std::string bytes;
    for (int index = first; index < first + count; ++index) {
        std::string line = "line " + std::to_string(index);
        line.resize(std::max(line.size(), width), '.');
        bytes += line + "\r\n";
    }
    return bytes;
}

// A client that backfills nothing on its own: its cache holds what it wants.
ClientPolicy backfillingNothing()
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.backfillDepthLines = 0;
    return policy;
}

ClientPolicy backfilling(std::uint32_t depth)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.backfillDepthLines = depth;
    return policy;
}

SyncLink linkWith(const ClientPolicy& policy)
{
    return SyncLink(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
}

// Binds a pane whose history holds every line it was written before.
std::uint32_t deepPane(SyncLink& link, std::string_view text)
{
    const std::uint32_t pane = link.addPane(24, 80, text, 50'000);
    link.stepUntilQuiet();
    return pane;
}

Ubiquitty::Screen::HistoryAddress addressOf(const SyncLink& link, std::uint32_t pane)
{
    return Ubiquitty::Screen::historyAddress(link.hostTerminal(pane));
}

const HistoryCache& cacheOf(const SyncLink& link, std::uint32_t pane)
{
    return *link.client().historyCache(pane);
}

// Whether the client holds every line of a range as the host does.
bool holdsAsTheHostDoes(const SyncLink& link, std::uint32_t pane, LineRange range)
{
    if (!cacheOf(link, pane).gaps(range).empty())
        return false;
    for (std::uint64_t number = range.from; number < range.to; ++number) {
        if (logicalLineHash(link.hostTerminal(pane), number) != cacheOf(link, pane).line(number)->lineHash)
            return false;
    }
    return true;
}

void sendWant(SyncLink& link, std::uint32_t pane, const HistoryWant& want)
{
    std::vector<std::uint8_t> body;
    encode(body, want);
    link.sendToHost(FrameKind::HistoryWant, body, pane);
}

} // namespace

// A want is answered: the lines a client asks for arrive. A thousand wants,
// for distinct viewports, with no service between them, leave at most one
// transfer, and the cache ends holding exactly the last one's lines.
TEST(aThousandRapidWantsLeaveAtMostOneTransferPerPriority)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = deepPane(link, lines(0, 3'000));
    const std::uint64_t head = addressOf(link, pane).head;
    REQUIRE(cacheOf(link, pane).lineCount() == 0);
    LineRange last;
    for (std::uint64_t index = 0; index < 1'000; ++index) {
        last = { head - 24 - 2 * index, head - 2 * index };
        const std::vector<LineRange> wanted { last };
        REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, wanted));
    }
    for (int step = 0; step < 200; ++step) {
        link.step();
        CHECK(link.host().historyTransfers(pane, HistoryPriority::Viewport) <= 1);
    }
    link.stepUntilQuiet();
    CHECK((cacheOf(link, pane).cachedRanges() == std::vector<LineRange> { last }));
    CHECK(holdsAsTheHostDoes(link, pane, last));
    CHECK(link.host().wantedHistory(pane, HistoryPriority::Viewport).empty());
}

// A client scrolled far above what it was backfilled fetches the lines it
// looks at.
TEST(onDemandFetchFillsAGap)
{
    SyncLink link = linkWith(backfilling(100));
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 790));
    link.stepUntilQuiet();
    const std::uint64_t head = addressOf(link, pane).head;
    const LineRange viewport { head - 500, head - 476 };
    REQUIRE(cacheOf(link, pane).gaps(viewport) == std::vector<LineRange> { viewport });
    const std::vector<LineRange> wanted { viewport };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, wanted));
    link.stepUntilQuiet();
    CHECK(holdsAsTheHostDoes(link, pane, viewport));
}

// A want that no longer covers a transfer whose head has left aborts it: the
// client is told, and fences what of it is still to come.
TEST(aNewViewportWantAbortsTheTransferItNoLongerCovers)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = deepPane(link, lines(0, 1'000, 400));
    const std::uint64_t head = addressOf(link, pane).head;
    const std::uint64_t abortsBefore = link.host().counters().bulkAborts;
    const std::vector<LineRange> first { { head - 600, head - 400 } };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, first));
    link.step();
    link.holdDownstream();
    link.step();
    REQUIRE(link.host().historyTransfers(pane, HistoryPriority::Viewport) == 1);
    REQUIRE(!link.host().queuedHistoryTransfers(pane).empty() && link.host().queuedHistoryTransfers(pane).front().isAnnounced);
    const std::vector<LineRange> second { { head - 100, head - 76 } };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, second));
    link.releaseDownstream();
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().bulkAborts, abortsBefore + 1);
    CHECK(holdsAsTheHostDoes(link, pane, second.front()));
    CHECK((cacheOf(link, pane).cachedRanges() == second));
}

// After a clear, a want below the new epoch's first line names nothing, and a
// want naming the old epoch is not this epoch's to serve.
TEST(aFetchBelowClearedThroughReturnsNothing)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 300));
    link.stepUntilQuiet();
    link.write(pane, "\x1b[3J" + lines(300, 100));
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress address = addressOf(link, pane);
    REQUIRE(address.epoch == 2 && address.clearedThrough > 0);
    const std::uint64_t linesBefore = link.host().counters().historyLinesSent;
    const std::vector<LineRange> cleared { { 0, address.clearedThrough } };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, cleared));
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyLinesSent, linesBefore);

    sendWant(link, pane, HistoryWant { .historyEpoch = 1, .ranges = { { address.clearedThrough, address.head } }, .priority = HistoryPriority::Viewport });
    CHECK(link.host().wantedHistory(pane, HistoryPriority::Viewport).empty());
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyLinesSent, linesBefore);
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// Ranges over the cap, summed over every priority, refuse the want whole and
// silently: the sets stand, nothing is queued for it, and the client stays.
TEST(aWantOverTheSummedLimitIsRefusedWholeAndTheSetsStand)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 790));
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress address = addressOf(link, pane);
    HistoryWant search { .historyEpoch = address.epoch, .ranges = {}, .priority = HistoryPriority::Search };
    for (std::uint64_t index = 0; index < 40; ++index)
        search.ranges.push_back({ address.oldest + 4 * index, address.oldest + 4 * index + 1 });
    HistoryWant viewport { .historyEpoch = address.epoch, .ranges = {}, .priority = HistoryPriority::Viewport };
    for (std::uint64_t index = 0; index < 30; ++index)
        viewport.ranges.push_back({ address.oldest + 200 + 4 * index, address.oldest + 200 + 4 * index + 1 });
    sendWant(link, pane, search);
    sendWant(link, pane, viewport);
    CHECK_EQ(link.host().wantedHistory(pane, HistoryPriority::Search).size(), std::size_t { 40 });
    CHECK(link.host().wantedHistory(pane, HistoryPriority::Viewport).empty());
    CHECK_EQ(link.host().counters().historyWantsRefused, std::uint64_t { 1 });
    link.step();
    CHECK_EQ(link.host().historyTransfers(pane, HistoryPriority::Viewport), std::size_t { 0 });
    CHECK(link.host().state() == ConnectionState::Attached);

    // The client's own sets never send a want past the cap.
    const std::vector<LineRange> tooMany(viewport.ranges.begin(), viewport.ranges.end());
    const std::vector<LineRange> forty(search.ranges.begin(), search.ranges.end());
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Search, forty));
    CHECK(!link.client().wantHistory(pane, HistoryPriority::Viewport, tooMany));
}

// A want for every line there could be is clipped to the history and served
// one capped transfer at a time.
TEST(aWantForTheWholeLineSpaceQueuesOneCappedTransfer)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = deepPane(link, lines(0, 3'000));
    const std::uint64_t linesBefore = link.host().counters().historyLinesSent;
    sendWant(link, pane, HistoryWant { .historyEpoch = addressOf(link, pane).epoch, .ranges = { { 0, std::numeric_limits<std::uint64_t>::max() } }, .priority = HistoryPriority::Viewport });
    link.serviceWithoutCarrying();
    CHECK_EQ(link.host().historyTransfers(pane, HistoryPriority::Viewport), std::size_t { 1 });
    CHECK(link.host().counters().historyLinesSent - linesBefore <= provisionalHistoryPolicy.backfillChunkLines);
    CHECK(link.host().queuedBulkBytes() <= provisionalHistoryPolicy.maxQueuedBulkBytes);
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress address = addressOf(link, pane);
    CHECK(holdsAsTheHostDoes(link, pane, { address.oldest, address.head }));
}

// A backfill in progress holds back neither a viewport nor a search: each is
// served while backfill still has lines to send.
TEST(viewportAndSearchWantsAreNeverRateLimited)
{
    SyncLink link = linkWith(backfilling(3'000));
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 3'000), 50'000);
    for (int step = 0; step < 200 && link.host().counters().historyLinesSent == 0; ++step)
        link.step();
    REQUIRE(link.host().counters().historyLinesSent > 0);
    const std::uint64_t head = addressOf(link, pane).head;
    const LineRange viewport { head - 2'900, head - 2'876 };
    const LineRange search { head - 2'800, head - 2'799 };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, std::vector<LineRange> { viewport }));
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Search, std::vector<LineRange> { search }));
    int steps = 0;
    while (steps < 50 && !(holdsAsTheHostDoes(link, pane, viewport) && holdsAsTheHostDoes(link, pane, search))) {
        link.step();
        ++steps;
    }
    CHECK(holdsAsTheHostDoes(link, pane, viewport));
    CHECK(holdsAsTheHostDoes(link, pane, search));
    CHECK(!link.host().wantedHistory(pane, HistoryPriority::Backfill).empty());
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(pane, 3'000), "");
}

// The bulk a flood of wants queues is what its last want queues.
TEST(tenThousandWantsKeepQueuedBulkFlat)
{
    SyncLink link = linkWith(backfillingNothing());
    const std::uint32_t pane = deepPane(link, lines(0, 3'000, 70));
    const std::uint64_t head = addressOf(link, pane).head;
    const auto flood = [&](int wants) -> std::size_t {
        for (int index = 0; index < wants; ++index) {
            const std::uint64_t to = head - static_cast<std::uint64_t>(index % 2'000);
            const std::vector<LineRange> wanted { { to - 200, to } };
            CHECK(link.client().wantHistory(pane, HistoryPriority::Viewport, wanted));
        }
        const std::vector<LineRange> last { { head - 2'500, head - 2'300 } };
        CHECK(link.client().wantHistory(pane, HistoryPriority::Viewport, last));
        link.step();
        link.serviceWithoutCarrying();
        const std::size_t queued = link.host().queuedBulkBytes();
        link.stepUntilQuiet();
        return queued;
    };
    const std::size_t afterTen = flood(10);
    const std::size_t afterTenThousand = flood(10'000);
    CHECK(afterTen > 0);
    CHECK_EQ(afterTenThousand, afterTen);
}

// The sets themselves: a refused want leaves every set as it was, a want of
// another epoch is ignored, and a range is clipped to what the host holds.
TEST(theWantSetsClipAndRefuseWhole)
{
    HistoryWants wants(4);
    const Ubiquitty::Screen::HistoryAddress address { .epoch = 3, .oldest = 100, .head = 500, .clearedThrough = 150 };
    CHECK(wants.replace({ .historyEpoch = 3, .ranges = { { 0, 160 }, { 160, 170 }, { 400, 900 } }, .priority = HistoryPriority::Search }, address) == WantOutcome::Accepted);
    CHECK((std::vector<LineRange>(wants.ranges(HistoryPriority::Search).begin(), wants.ranges(HistoryPriority::Search).end()) == std::vector<LineRange> { { 150, 170 }, { 400, 500 } }));
    CHECK(wants.replace({ .historyEpoch = 3, .ranges = { { 200, 201 }, { 202, 203 }, { 204, 205 } }, .priority = HistoryPriority::Viewport }, address) == WantOutcome::RefusedOverLimit);
    CHECK(wants.ranges(HistoryPriority::Viewport).empty());
    CHECK_EQ(wants.pendingRanges(), std::size_t { 2 });
    CHECK(wants.replace({ .historyEpoch = 2, .ranges = { { 200, 201 } }, .priority = HistoryPriority::Viewport }, address) == WantOutcome::StaleEpoch);
    CHECK(wants.ranges(HistoryPriority::Viewport).empty());
    CHECK((wants.nextSlice(HistoryPriority::Search, 8) == LineRange { 150, 158 }));
    wants.exclude({ 150, 158 });
    CHECK((wants.nextSlice(HistoryPriority::Search, 100) == LineRange { 158, 170 }));
    CHECK(wants.replace({ .historyEpoch = 3, .ranges = { { 300, 450 } }, .priority = HistoryPriority::Backfill }, address) == WantOutcome::Accepted);
    CHECK((wants.nextSlice(HistoryPriority::Backfill, 100) == LineRange { 350, 450 }));
    CHECK(wants.intersectsAny({ 449, 460 }));
    CHECK(!wants.intersectsAny({ 170, 300 }));
}

namespace {

/// A host that serves `pane.search` for a link's panes, as the daemon does.
///
/// - Parameter link: The link.
/// - Returns: The server.
CallServer searchServer(SyncLink& link)
{
    return [&link](const Ubiquitty::Protocol::Control::DecodedRequest& request) {
        const Ubiquitty::Codec::JSON::Value* pane = request.params.find("pane");
        return Ubiquitty::Protocol::Sync::searchPane(link.hostTerminal(pane ? static_cast<std::uint32_t>(pane->asNumber()) : link.handle()), request);
    };
}

/// A `pane.search` request.
///
/// - Parameters:
///   - id: Its id.
///   - pane: The pane's handle.
///   - pattern: The pattern.
/// - Returns: Its text.
std::string searchRequest(int id, std::uint32_t pane, std::string_view pattern)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"pane.search","params":{"pane":)" + std::to_string(pane) + R"(,"pattern":")"
        + std::string(pattern) + R"("}})";
}

} // namespace

// On a metered link, which pushes one viewport,
// a search finds a line the client never fetched, by its number; a Search
// want then brings it.
TEST(aSearchFindsALineTheClientNeverFetched)
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.link = LinkCost::Metered;
    SyncLink link(0, facts);
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 3'000), 10'000);
    link.stepUntilQuiet();
    const std::uint64_t needle = 42;
    REQUIRE(cacheOf(link, pane).line(needle) == nullptr);

    REQUIRE(link.client().call(searchRequest(1, pane, "^line 42$")));
    link.step();
    serveCalls(link.host(), searchServer(link));
    link.stepUntilQuiet();
    const std::vector<std::string> results = link.client().takeResults();
    REQUIRE(results.size() == 1);
    bool ok = false;
    const Ubiquitty::Codec::JSON::Value parsed = Ubiquitty::Codec::JSON::parse(results.front(), ok);
    REQUIRE(ok && parsed.find("result"));
    const Ubiquitty::Codec::JSON::Value& result = *parsed.find("result");
    REQUIRE(result.find("matches") && result.find("matches")->items().size() == 1);
    const Ubiquitty::Codec::JSON::Value& match = result.find("matches")->items().front();
    CHECK_EQ(static_cast<std::uint64_t>(match.numberOr("line")), needle);
    CHECK_EQ(static_cast<std::uint32_t>(result.numberOr("epoch")), addressOf(link, pane).epoch);
    CHECK(!result.boolOr("truncated", true));
    CHECK(cacheOf(link, pane).line(needle) == nullptr);

    const std::vector<LineRange> wanted { { needle, needle + 1 } };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Search, wanted));
    link.stepUntilQuiet();
    CHECK(holdsAsTheHostDoes(link, pane, wanted.front()));
}

// Searches are calls, under the same cap: the thirty-third outstanding is
// answered busy and the connection stays.
TEST(theThirtyThirdSearchIsBusy)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 500), 10'000);
    link.stepUntilQuiet();
    for (int id = 1; id <= 33; ++id)
        REQUIRE(link.client().call(searchRequest(id, pane, "line")));
    link.step();
    std::vector<ReceivedCall> admitted;
    for (ReceivedCall& call : link.host().takeCalls()) {
        if (call.admission == CallAdmission::OverLimit)
            link.host().refuseCall(call.callIndex, busyResponse(nullptr), CallRefusal::Busy);
        else
            admitted.push_back(std::move(call));
    }
    CHECK_EQ(admitted.size(), std::size_t { 32 });
    link.stepUntilQuiet();
    const std::vector<std::string> results = link.client().takeResults();
    REQUIRE(results.size() == 1);
    CHECK(results.front().find("-32005") != std::string::npos);
    CHECK(link.host().state() == ConnectionState::Attached);
}

UBIQUITTY_TEST_MAIN()
