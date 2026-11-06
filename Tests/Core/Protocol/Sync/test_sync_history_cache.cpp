// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history_cache.cpp — what a client keeps of a pane's history
// (Docs/sync.md §6.6): lines of one epoch, keyed by the host and the pane,
// under the client's own cap, re-wrapped at the client's own width.
//
// - A reset leaves nothing of the old epoch, and no line of it is taken after.
// - Nothing below a reset's `clearedThrough` is taken.
// - A head that retreats hides the lines above it, and restores them only if
//   no line of the same number arrived while they were hidden.
// - A line older than the host's oldest is kept, flagged, and never wanted.
// - The cap evicts the oldest line first.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr ID128 hostInstance { .low = 1, .high = 2 };
constexpr ID128 pane { .low = 3, .high = 4 };

DecodedLine lineOf(std::uint64_t number, const std::string& text)
{
    DecodedLine line;
    line.number = number;
    for (const char character : text) {
        Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
        cell.codePoint = static_cast<std::uint32_t>(character);
        line.cells.push_back(cell);
    }
    line.lineHash = number * 31 + text.size();
    return line;
}

std::vector<DecodedLine> linesOf(std::uint64_t from, std::uint64_t to, const std::string& prefix = "line ")
{
    std::vector<DecodedLine> lines;
    for (std::uint64_t number = from; number < to; ++number)
        lines.push_back(lineOf(number, prefix + std::to_string(number)));
    return lines;
}

std::string textOf(const Ubiquitty::Screen::ScrollLine& row)
{
    std::string text;
    for (int column = 0; column < row.length; ++column) {
        if (row.cells[column].codePoint >= 0x20 && row.cells[column].codePoint < 0x7F)
            text.push_back(static_cast<char>(row.cells[column].codePoint));
    }
    return text;
}

} // namespace

TEST(aHandoverKeepsHistoryUnderTheSuccessorsKey)
{
    const ID128 successor { 5, 6 };
    ReplicaSet replicas;
    CHECK(!replicas.noteHostInstance(hostInstance, std::nullopt));
    auto cache = std::make_shared<HistoryCache>(hostInstance, pane, 1000);
    REQUIRE(cache->reset({ .newEpoch = 7, .clearedThrough = 20, .historyHead = 520 }) == HistoryPurgeOutcome::Purged);
    REQUIRE(cache->insert(7, linesOf(20, 520)) == CacheInsert::Stored);
    cache->noteHostOldest(30);
    auto open = lineOf(520, "unfinished");
    open.flags = historyLineOpen;
    REQUIRE(cache->setOpenLine(7, std::move(open)) == CacheInsert::Stored);
    REQUIRE(replicas.keepHistory(cache));
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 24, 80);
    cache->project(*terminal, 80);
    const auto* picture = terminal.get();
    (void)replicas.hold(pane, std::move(terminal));
    const auto ranges = cache->cachedRanges();

    CHECK(!replicas.noteHostInstance(successor, hostInstance));
    REQUIRE(replicas.history(pane) == cache);
    CHECK(cache->key().hostInstanceID == successor);
    CHECK(cache->key().paneID == pane);
    CHECK_EQ(cache->key().epoch, 7u);
    CHECK_EQ(cache->lineCount(), 500u);
    CHECK(cache->cachedRanges() == ranges);
    CHECK(cache->isBeyondHostHorizon(25));
    REQUIRE(cache->openLine());
    CHECK_EQ(cache->openLine()->number, 520u);
    CHECK(replicas.terminal(pane) == picture);
    CHECK(!replicas.noteHostInstance(successor, hostInstance));
    CHECK(replicas.history(pane) == cache);
}

TEST(anUnrelatedHostNeverReceivesTheOldHistory)
{
    for (const auto predecessor : { std::optional<ID128> {}, std::optional { ID128 { 90, 91 } } }) {
        ReplicaSet replicas;
        CHECK(!replicas.noteHostInstance(hostInstance, std::nullopt));
        auto cache = std::make_shared<HistoryCache>(hostInstance, pane, 1000);
        REQUIRE(cache->insert(1, linesOf(0, 500)) == CacheInsert::Stored);
        REQUIRE(replicas.keepHistory(cache));
        CHECK(replicas.noteHostInstance({ 5, 6 }, predecessor));
        CHECK(!replicas.history(pane));
        CHECK(cache->key().hostInstanceID == hostInstance);
    }
}

TEST(aHistoryRekeyPreservesHiddenLinesAndRejectsUnknownIdentity)
{
    HistoryCache cache(hostInstance, pane, 1000);
    REQUIRE(cache.insert(1, linesOf(0, 500)) == CacheInsert::Stored);
    cache.setHead(1, 500);
    cache.setHead(1, 480);
    const auto* retained = cache.line(20);
    REQUIRE(retained);
    CHECK(!cache.rekeyForHandover({ 99, 100 }, { 5, 6 }));
    CHECK(!cache.rekeyForHandover(hostInstance, {}));
    CHECK(!cache.rekeyForHandover({}, { 5, 6 }));
    CHECK(cache.key().hostInstanceID == hostInstance);
    CHECK_EQ(cache.lineCount(), 480u);
    REQUIRE(cache.rekeyForHandover(hostInstance, { 5, 6 }));
    CHECK(cache.line(20) == retained);
    CHECK(cache.line(490) == nullptr);
    cache.setHead(1, 500);
    CHECK_EQ(cache.lineCount(), 500u);
    CHECK(cache.line(490));
}

TEST(aHandedOverHistorySnapshotBelongsOnlyToTheSuccessor)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(terminal.get(), 100);
    Ubiquitty::resize(*terminal, 4, 32);
    for (int index = 0; index < 12; ++index)
        UbiquittyTest::feed(*terminal, "\033[32mhistory-" + std::to_string(index) + " e\xcc\x81\033[0m\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*terminal));
    const auto address = publisher.address();
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.writeBody(*terminal, { address.oldest, address.head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) > 2);
    std::vector<DecodedLine> lines;
    REQUIRE(decodeHistoryBody(body, address.oldest, HistoryOrder::Ascending, historyBodyLimits, lines));
    HistoryCache cache(hostInstance, pane, 100);
    REQUIRE(cache.reset({ .newEpoch = address.epoch, .clearedThrough = address.oldest, .historyHead = address.head }) == HistoryPurgeOutcome::Purged);
    REQUIRE(cache.insert(address.epoch, lines) == CacheInsert::Stored);
    REQUIRE(cache.rekeyForHandover(hostInstance, { 5, 6 }));
    constexpr std::size_t budget = 1 << 20;
    const auto snapshot = cache.snapshot(budget, budget);
    REQUIRE(snapshot);
    HistoryCache newKey({ 5, 6 }, pane, 100);
    REQUIRE(newKey.restoreSnapshot(snapshot->bytes, budget, budget));
    CHECK(newKey.cachedRanges() == cache.cachedRanges());
    for (const auto& line : lines) {
        REQUIRE(newKey.line(line.number));
        CHECK_EQ(newKey.line(line.number)->contentHash(), line.contentHash());
    }
    HistoryCache oldKey(hostInstance, pane, 100);
    CHECK(!oldKey.restoreSnapshot(snapshot->bytes, budget, budget));
}

// A reset is a purge. Nothing of the old epoch stays, and the
// old epoch's lines are refused afterwards, however late they arrive.
TEST(aResetLeavesNoLineOfTheOldEpoch)
{
    HistoryCache cache(hostInstance, pane, 10'000);
    const std::vector<DecodedLine> lines = linesOf(0, 300);
    CHECK(cache.insert(1, lines) == CacheInsert::Stored);
    CHECK_EQ(cache.lineCount(), std::size_t { 300 });

    CHECK(cache.reset(HistoryReset { .newEpoch = 2, .clearedThrough = 300, .historyHead = 300 }) == HistoryPurgeOutcome::Purged);
    CHECK_EQ(cache.lineCount(), std::size_t { 0 });
    CHECK(cache.insert(1, lines) == CacheInsert::WrongEpoch);
    CHECK_EQ(cache.lineCount(), std::size_t { 0 });
    CHECK(cache.cachedRanges().empty());
    CHECK_EQ(cache.key().epoch, 2u);
    CHECK(cache.key().hostInstanceID == hostInstance);
    CHECK(cache.key().paneID == pane);
}

TEST(aLineBelowClearedThroughIsRefused)
{
    HistoryCache cache(hostInstance, pane, 10'000);
    CHECK(cache.reset(HistoryReset { .newEpoch = 2, .clearedThrough = 300, .historyHead = 310 }) == HistoryPurgeOutcome::Purged);
    CHECK(cache.insert(2, linesOf(290, 310)) == CacheInsert::BelowClearedThrough);
    CHECK_EQ(cache.lineCount(), std::size_t { 0 });
    CHECK(cache.insert(2, linesOf(300, 310)) == CacheInsert::Stored);
    CHECK_EQ(cache.lineCount(), std::size_t { 10 });
}

TEST(aLogicalLineWrapsAtTheClientsWidth)
{
    HistoryCache cache(hostInstance, pane, 10'000);
    const std::vector<DecodedLine> lines { lineOf(0, std::string(250, 'q')), lineOf(1, "short") };
    CHECK(cache.insert(1, lines) == CacheInsert::Stored);
    cache.setHead(1, 2);

    auto replica = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*replica, 24, 80);
    cache.project(*replica, 80);
    REQUIRE(Ubiquitty::Screen::scrollbackCount(replica.get()) == 5);
    for (int index = 0; index < 3; ++index)
        CHECK(Ubiquitty::Screen::scrollbackLine(replica.get(), index)->wrapped);
    CHECK(!Ubiquitty::Screen::scrollbackLine(replica.get(), 3)->wrapped);
    CHECK_EQ(Ubiquitty::Screen::scrollbackLine(replica.get(), 3)->length, 10);
    CHECK_STR_EQ(textOf(*Ubiquitty::Screen::scrollbackLine(replica.get(), 4)), "short");

    cache.project(*replica, 100);
    REQUIRE(Ubiquitty::Screen::scrollbackCount(replica.get()) == 4);
    CHECK(Ubiquitty::Screen::scrollbackLine(replica.get(), 1)->wrapped);
    CHECK(!Ubiquitty::Screen::scrollbackLine(replica.get(), 2)->wrapped);
    CHECK_EQ(Ubiquitty::Screen::scrollbackLine(replica.get(), 2)->length, 50);
}

// A wide character never straddles the client's margin: it moves whole to the
// next row, as the Core wraps it.
TEST(aWideCharacterAtTheClientsMarginMovesWhole)
{
    DecodedLine line = lineOf(0, std::string(9, 'a'));
    Ubiquitty::Cell lead = Ubiquitty::defaultBlankCell;
    lead.codePoint = 0x4E16;
    lead.pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    line.cells.push_back(lead);
    line.cells.push_back(Ubiquitty::Screen::derivedContinuation(lead));
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, std::vector<DecodedLine> { line }) == CacheInsert::Stored);
    cache.setHead(1, 1);
    auto replica = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*replica, 5, 10);
    cache.project(*replica, 10);
    REQUIRE(Ubiquitty::Screen::scrollbackCount(replica.get()) == 2);
    CHECK(Ubiquitty::Screen::scrollbackLine(replica.get(), 0)->wrapped);
    CHECK_EQ(Ubiquitty::Screen::scrollbackLine(replica.get(), 1)->cells[0].codePoint, 0x4E16u);
}

TEST(replacingAnOpenHistoryLineDoesNotMoveAHeldViewport)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, linesOf(0, 5)) == CacheInsert::Stored);
    cache.setHead(1, 5);
    CHECK(cache.setOpenLine(1, lineOf(5, "open line grows")) == CacheInsert::Stored);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 10);
    cache.project(*replica, 10);
    Selection::Session selection;
    Selection::init(selection);
    constexpr int heldOffset = 3;
    Selection::observeViewport(selection, *replica, heldOffset);
    const auto before = Selection::updateAfterOutput(selection, *replica, heldOffset, Input::OutputSnapPolicy::HoldViewedRows);
    REQUIRE(before.viewport.viewOffset == heldOffset);
    const auto viewed = textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - heldOffset));

    auto edited = lineOf(5, "open line edits");
    ++edited.lineHash;
    CHECK(cache.setOpenLine(1, std::move(edited)) == CacheInsert::Stored);
    cache.project(*replica, 10);
    const auto after = Selection::updateAfterOutput(selection, *replica, heldOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(after.viewport.viewOffset, heldOffset);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - after.viewport.viewOffset)), viewed);
}

TEST(olderBackfillDoesNotMoveAHeldViewportButNewOutputDoes)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, linesOf(5, 10)) == CacheInsert::Stored);
    cache.setHead(1, 10);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 20);
    cache.project(*replica, 20);
    Selection::Session selection;
    Selection::init(selection);
    constexpr int heldOffset = 2;
    Selection::observeViewport(selection, *replica, heldOffset);
    (void)Selection::updateAfterOutput(selection, *replica, heldOffset, Input::OutputSnapPolicy::HoldViewedRows);
    const auto viewed = textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - heldOffset));

    CHECK(cache.insert(1, linesOf(0, 5)) == CacheInsert::Stored);
    cache.project(*replica, 20);
    const auto backfill = Selection::updateAfterOutput(selection, *replica, heldOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(backfill.viewport.viewOffset, heldOffset);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - backfill.viewport.viewOffset)), viewed);

    CHECK(cache.insert(1, linesOf(10, 11)) == CacheInsert::Stored);
    cache.setHead(1, 11);
    cache.project(*replica, 20);
    const auto appended = Selection::updateAfterOutput(selection, *replica, heldOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(appended.viewport.viewOffset, heldOffset + 1);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - appended.viewport.viewOffset)), viewed);
}

TEST(aHeldLogicalPositionSurvivesRewrapAndReturnsToTheSameText)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, std::vector<DecodedLine> { lineOf(30, "first"), lineOf(31, "0123456789ABCDEFGHIJklmnopqrstUVWXYZ"), lineOf(32, "last") }) == CacheInsert::Stored);
    cache.setHead(1, 33);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 10);
    cache.project(*replica, 10);
    Selection::Session selection;
    Selection::init(selection);
    int offset = replica->scrollbackCount - 3;
    Selection::observeViewport(selection, *replica, offset);
    REQUIRE(selection.viewportAnchor);
    CHECK(Ubiquitty::Screen::lineNumberOf(*replica, selection.viewportAnchor->line) == std::optional<std::uint64_t> { 31 });
    CHECK_EQ(selection.viewportAnchor->cell, 20u);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), 3)), "klmnopqrst");

    cache.project(*replica, 7);
    auto update = Selection::updateAfterOutput(selection, *replica, offset, Input::OutputSnapPolicy::HoldViewedRows);
    offset = static_cast<int>(update.viewport.viewOffset);
    CHECK_EQ(replica->scrollbackCount - offset, 3);
    REQUIRE(selection.viewportAnchor);
    CHECK_EQ(selection.viewportAnchor->cell, 20u);
    cache.project(*replica, 10);
    update = Selection::updateAfterOutput(selection, *replica, offset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - update.viewport.viewOffset)), "klmnopqrst");
}

TEST(aHeldViewportClampsToSurvivingHistoryAfterEvictionAndPurge)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, linesOf(0, 6)) == CacheInsert::Stored);
    cache.setHead(1, 6);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 20);
    replica->scrollbackLimit = 3;
    cache.project(*replica, 20);
    REQUIRE(replica->scrollbackCount == 3);
    Selection::Session selection;
    Selection::init(selection);
    Selection::observeViewport(selection, *replica, 2);
    REQUIRE(selection.viewportAnchor);
    CHECK(Ubiquitty::Screen::lineNumberOf(*replica, selection.viewportAnchor->line) == std::optional<std::uint64_t> { 4 });
    CHECK(cache.insert(1, linesOf(6, 8)) == CacheInsert::Stored);
    cache.setHead(1, 8);
    cache.project(*replica, 20);
    auto update = Selection::updateAfterOutput(selection, *replica, 2, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(update.viewport.viewOffset, 3);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), 0)), "line 5");

    CHECK(cache.reset(HistoryReset { .newEpoch = 2, .clearedThrough = 20, .historyHead = 23 }) == HistoryPurgeOutcome::Purged);
    CHECK(cache.insert(2, linesOf(20, 23)) == CacheInsert::Stored);
    cache.project(*replica, 20);
    update = Selection::updateAfterOutput(selection, *replica, 3, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(update.viewport.viewOffset, 3);
    REQUIRE(selection.viewportAnchor);
    CHECK(Ubiquitty::Screen::lineNumberOf(*replica, selection.viewportAnchor->line) == std::optional<std::uint64_t> { 20 });
    CHECK_EQ(selection.viewportHistoryEpoch, 2u);

    Selection::observeViewport(selection, *replica, 0);
    CHECK(cache.insert(2, linesOf(23, 24)) == CacheInsert::Stored);
    cache.setHead(2, 24);
    cache.project(*replica, 20);
    update = Selection::updateAfterOutput(selection, *replica, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(update.viewport.viewOffset, 0);
    CHECK(update.viewport.hold == Input::ViewportHold::Follow);
    CHECK(!selection.viewportAnchor);
}

TEST(anOpenOnlyProjectionKeepsItsHostLogicalNumber)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    cache.setHead(1, 100);
    CHECK(cache.setOpenLine(1, lineOf(100, "0123456789ABCDEFGHIJklmnopqrst")) == CacheInsert::Stored);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 10);
    cache.project(*replica, 10);
    Selection::Session selection;
    Selection::init(selection);
    Selection::observeViewport(selection, *replica, 2);
    REQUIRE(selection.viewportAnchor);
    CHECK(Ubiquitty::Screen::lineNumberOf(*replica, selection.viewportAnchor->line) == std::optional<std::uint64_t> { 100 });
    CHECK_EQ(selection.viewportAnchor->cell, 10u);
    CHECK(cache.insert(1, std::vector<DecodedLine> { lineOf(100, "0123456789ABCDEFGHIJklmnopqrst") }) == CacheInsert::Stored);
    cache.setHead(1, 101);
    cache.project(*replica, 10);
    const auto update = Selection::updateAfterOutput(selection, *replica, 2, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(update.viewport.viewOffset, 2);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - update.viewport.viewOffset)), "ABCDEFGHIJ");
}

TEST(aProjectionTruncatedWithinOneLineDoesNotInventItsMissingPrefix)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    cache.setHead(1, 101);
    CHECK(cache.insert(1, std::vector<DecodedLine> { lineOf(100, std::string(60, 'x')) }) == CacheInsert::Stored);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 10);
    replica->scrollbackLimit = 3;
    cache.project(*replica, 10);
    REQUIRE(replica->lineStore);
    CHECK_EQ(replica->lineStore->frontLine().number, 100u);
    CHECK(replica->lineStore->frontLine().continuity == LineStore::LineContinuity::ContinuesLine);
    CHECK(!replica->lineStore->positionAt(0));
    CHECK(!replica->lineStore->logicalLine(100));
}

TEST(independentHeldViewsResolveAgainstTheCurrentModelNotAQueuedSnapshot)
{
    using namespace Ubiquitty;
    HistoryCache cache(hostInstance, pane, 100);
    CHECK(cache.insert(1, linesOf(0, 10)) == CacheInsert::Stored);
    cache.setHead(1, 10);
    auto replica = std::make_unique<Terminal>();
    resize(*replica, 4, 20);
    cache.project(*replica, 20);
    Selection::Session nearView;
    Selection::Session olderView;
    Selection::init(nearView);
    Selection::init(olderView);
    Selection::observeViewport(nearView, *replica, 2);
    Selection::observeViewport(olderView, *replica, 5);
    const auto queuedFacts = Screen::historyFacts(replica.get());
    CHECK(cache.insert(1, linesOf(10, 12)) == CacheInsert::Stored);
    cache.setHead(1, 12);
    cache.project(*replica, 20);
    std::mutex modelLock;
    const auto nearUpdate = Selection::updateAfterOutput(nearView, *replica, modelLock, 2, queuedFacts);
    const auto olderUpdate = Selection::updateAfterOutput(olderView, *replica, modelLock, 5, queuedFacts);
    CHECK_EQ(nearUpdate.viewport.viewOffset, 4);
    CHECK_EQ(olderUpdate.viewport.viewOffset, 7);
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - nearUpdate.viewport.viewOffset)), "line 8");
    CHECK_STR_EQ(textOf(*Screen::scrollbackLine(replica.get(), replica->scrollbackCount - olderUpdate.viewport.viewOffset)), "line 5");

    Selection::observeViewport(nearView, *replica, 1);
    const auto moved = Selection::updateAfterOutput(nearView, *replica, modelLock, 1, queuedFacts);
    CHECK_EQ(moved.viewport.viewOffset, 1);
    Selection::init(nearView);
    CHECK(!nearView.usesLogicalViewport);
    CHECK(!nearView.viewportAnchor);
}

TEST(aWithdrawnLineIsRestoredOnlyWhenNothingReplacedIt)
{
    HistoryCache cache(hostInstance, pane, 10'000);
    CHECK(cache.insert(1, linesOf(0, 100)) == CacheInsert::Stored);
    cache.setHead(1, 100);
    CHECK_EQ(cache.contiguousHead(), 100u);

    // A taller screen pulled five lines back: they are hidden, not dropped.
    cache.setHead(1, 95);
    CHECK(!cache.line(97));
    CHECK_EQ(cache.contiguousHead(), 95u);
    CHECK_EQ(cache.lineCount(), std::size_t { 95 });

    // Line 97 came back changed; 95, 96, 98 and 99 come back as they were.
    CHECK(cache.insert(1, std::vector<DecodedLine> { lineOf(97, "edited") }) == CacheInsert::Stored);
    cache.setHead(1, 100);
    REQUIRE(cache.line(96));
    CHECK_EQ(cache.line(96)->lineHash, lineOf(96, "line 96").lineHash);
    REQUIRE(cache.line(97));
    CHECK_EQ(cache.line(97)->lineHash, lineOf(97, "edited").lineHash);
    CHECK_EQ(cache.contiguousHead(), 100u);
    CHECK_EQ(cache.lineCount(), std::size_t { 100 });
}

TEST(aGapNeverNamesALineBeyondTheHostHorizon)
{
    HistoryCache cache(hostInstance, pane, 10'000);
    CHECK(cache.insert(1, linesOf(100, 200)) == CacheInsert::Stored);
    CHECK(cache.insert(1, linesOf(300, 400)) == CacheInsert::Stored);
    cache.setHead(1, 400);
    CHECK((cache.gaps({ 0, 400 }) == std::vector<LineRange> { { 0, 100 }, { 200, 300 } }));

    // The host's ring has moved past 250: what the client holds below it stays,
    // flagged, and the part of the gap below it is not one the host can fill.
    cache.noteHostOldest(250);
    CHECK(cache.isBeyondHostHorizon(150));
    CHECK(!cache.isBeyondHostHorizon(300));
    REQUIRE(cache.line(150));
    CHECK((cache.gaps({ 0, 400 }) == std::vector<LineRange> { { 250, 300 } }));
    CHECK((cache.cachedRanges() == std::vector<LineRange> { { 100, 200 }, { 300, 400 } }));
    CHECK_EQ(cache.contiguousHead(), 400u);
}

TEST(theMaxLinesEvictsOldestFirst)
{
    HistoryCache cache(hostInstance, pane, 50);
    CHECK(cache.insert(1, linesOf(0, 80)) == CacheInsert::Stored);
    CHECK_EQ(cache.lineCount(), std::size_t { 50 });
    CHECK(!cache.line(29));
    CHECK(cache.line(30));
    CHECK(cache.line(79));
    CHECK(cache.insert(1, linesOf(80, 90)) == CacheInsert::Stored);
    CHECK(!cache.line(39));
    CHECK(cache.line(40));
}

// The purge seam: a sink is told the key the reset retired and the line it
// cleared through, for the copies the cache does not own.
TEST(aPurgeSinkHearsTheRetiredKey)
{
    struct RecordingSink final : HistoryPurgeSink {
        std::vector<std::pair<HistoryKey, std::uint64_t>> purges;
        HistoryPurgeOutcome purge(const HistoryKey& retired, std::uint64_t clearedThrough) override
        {
            purges.emplace_back(retired, clearedThrough);
            return HistoryPurgeOutcome::Purged;
        }
    };
    RecordingSink sink;
    HistoryCache cache(hostInstance, pane, 100);
    const HistoryKey before = cache.key();
    CHECK(cache.reset(HistoryReset { .newEpoch = 5, .clearedThrough = 40, .historyHead = 40 }, &sink) == HistoryPurgeOutcome::Purged);
    REQUIRE(sink.purges.size() == 1);
    CHECK(sink.purges[0].first == before);
    CHECK_EQ(sink.purges[0].second, 40u);
}

TEST(aRefusedPurgeLeavesTheCommittedCacheUntilRetirementSucceeds)
{
    struct RefusingSink final : HistoryPurgeSink {
        bool shouldRefuse = true;
        HistoryPurgeOutcome purge(const HistoryKey&, std::uint64_t) override
        {
            return shouldRefuse ? HistoryPurgeOutcome::Refused : HistoryPurgeOutcome::Purged;
        }
    } sink;
    HistoryCache cache(hostInstance, pane, 100);
    REQUIRE(cache.insert(1, linesOf(0, 40)) == CacheInsert::Stored);
    cache.setHead(1, 40);
    const HistoryKey before = cache.key();
    const auto ranges = cache.cachedRanges();
    const HistoryReset reset { .newEpoch = 2, .clearedThrough = 40, .historyHead = 40 };
    CHECK(cache.reset(reset, &sink) == HistoryPurgeOutcome::Refused);
    CHECK(cache.key() == before);
    CHECK(cache.cachedRanges() == ranges);
    CHECK_EQ(cache.lineCount(), std::size_t { 40 });
    CHECK_EQ(cache.contiguousHead(), 40u);
    sink.shouldRefuse = false;
    CHECK(cache.reset(reset, &sink) == HistoryPurgeOutcome::Purged);
    CHECK_EQ(cache.key().epoch, 2u);
    CHECK_EQ(cache.lineCount(), std::size_t { 0 });
}

TEST(aRetainedHistorySnapshotPreservesCanonicalLinesAndRejectsOtherOwners)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(terminal.get(), 100);
    Ubiquitty::resize(*terminal, 4, 32);
    for (int index = 0; index < 12; ++index)
        UbiquittyTest::feed(*terminal, "\033[32mhistory-" + std::to_string(index) + " e\xcc\x81\033[0m\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*terminal));
    const auto address = publisher.address();
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.writeBody(*terminal, { address.oldest, address.head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) > 2);
    std::vector<DecodedLine> lines;
    REQUIRE(decodeHistoryBody(body, address.oldest, HistoryOrder::Ascending, historyBodyLimits, lines));
    HistoryCache cache(hostInstance, pane, 100);
    REQUIRE(cache.reset({ .newEpoch = address.epoch, .clearedThrough = address.oldest, .historyHead = address.head }) == HistoryPurgeOutcome::Purged);
    REQUIRE(cache.insert(address.epoch, lines) == CacheInsert::Stored);
    constexpr std::size_t budget = 1 << 20;
    auto snapshot = cache.snapshot(budget, budget);
    REQUIRE(snapshot.has_value());
    HistoryCache restored(hostInstance, pane, 100);
    REQUIRE(restored.restoreSnapshot(snapshot->bytes, budget, budget));
    CHECK_EQ(restored.retainedStorageBytes(), snapshot->retainedBytes);
    CHECK(restored.cachedRanges() == cache.cachedRanges());
    CHECK_EQ(restored.head(), cache.head());
    for (const auto& line : lines) {
        REQUIRE(restored.line(line.number));
        CHECK_EQ(restored.line(line.number)->contentHash(), line.contentHash());
    }
    HistoryCache otherHost({ 9, 9 }, pane, 100);
    CHECK(!otherHost.restoreSnapshot(snapshot->bytes, budget, budget));
    HistoryCache otherPane(hostInstance, { 9, 9 }, 100);
    CHECK(!otherPane.restoreSnapshot(snapshot->bytes, budget, budget));
    const auto before = restored.cachedRanges();
    const auto capped = cache.snapshot(snapshot->bytes.size() / 2, budget);
    REQUIRE(capped);
    HistoryCache bounded(hostInstance, pane, 100);
    REQUIRE(bounded.restoreSnapshot(capped->bytes, budget, budget));
    CHECK(bounded.lineCount() > 0 && bounded.lineCount() < cache.lineCount());
    CHECK(bounded.line(address.head - 1));
    CHECK(!bounded.line(address.oldest));
    CHECK(!restored.restoreSnapshot(snapshot->bytes, snapshot->bytes.size() - 1, budget));
    CHECK(!restored.restoreSnapshot(snapshot->bytes, budget, 1));
    CHECK(restored.cachedRanges() == before);
    snapshot->bytes.back() ^= 0x80;
    CHECK(!restored.restoreSnapshot(snapshot->bytes, budget, budget));
    CHECK(restored.cachedRanges() == before);

    cache.setHead(address.epoch, address.head - 2);
    const auto pulled = cache.snapshot(budget, budget);
    REQUIRE(pulled.has_value());
    REQUIRE(restored.restoreSnapshot(pulled->bytes, budget, budget));
    CHECK(!restored.line(address.head - 1));
    CHECK(!restored.line(address.head - 2));
    CHECK_EQ(restored.head(), address.head - 2);
    REQUIRE(cache.reset({ .newEpoch = address.epoch + 1, .clearedThrough = address.head, .historyHead = address.head }) == HistoryPurgeOutcome::Purged);
    const auto cleared = cache.snapshot(budget, budget);
    REQUIRE(cleared.has_value());
    REQUIRE(restored.restoreSnapshot(cleared->bytes, budget, budget));
    CHECK(restored.cachedRanges().empty());
    CHECK_EQ(restored.key().epoch, address.epoch + 1);

    auto invalid = lines.back();
    invalid.lineHash ^= 1;
    HistoryCache corrupt(hostInstance, pane, 100);
    REQUIRE(corrupt.insert(1, { &invalid, 1 }) == CacheInsert::Stored);
    corrupt.setHead(1, invalid.number + 1);
    CHECK(!corrupt.snapshot(budget, budget));
}

UBIQUITTY_TEST_MAIN()
