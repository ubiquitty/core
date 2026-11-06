// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history.cpp — history on its own plane (Docs/sync.md §6.6, §7.5,
// §7.8), carried by both ends of a connection.
//
// - A resize sends no history: a checkpoint names the history's epoch and
//   head and carries no line, a taller screen that pulls lines back sends
//   nothing, and the lines a later shrink pushes out again cost nothing when
//   they are what they were.
// - The appends a publish made precede the update that scrolls them, inline
//   when they fit one frame and as a transfer when they do not.
// - An unmetered attach is backfilled newest first, to the depth.
// - History never travels as a datagram; an update that does may arrive ahead
//   of its appends, and the stream fills the gap.
// - A client is sent no more history than its `Greet` says it keeps, and one
//   that keeps none, as an editor's, is sent none but `HistoryReset`.
// - A metered client is pushed one viewport above the screen, and pulls the
//   rest; a link that turns metered stops the backfill, and one that turns
//   unmetered resumes it.
// - A line straddling the seam is in the client's history as the open line
//   from the frame its first row leaves the screen: replaced in place as it
//   grows and when it completes, kept once across a reflow, purged by a clear.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Cell.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
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

const HistoryCache& cacheOf(const SyncLink& link)
{
    return *link.client().historyCache(link.handle());
}

Ubiquitty::Screen::HistoryAddress hostAddress(const SyncLink& link)
{
    return Ubiquitty::Screen::historyAddress(link.hostTerminal());
}

static_assert(defaultHistoryCacheLines == Ubiquitty::Preferences::defaultScrollbackLines, "a client keeps as much history as a configured session's scrollback");

// How many of a terminal's history rows begin with `prefix`.
int historyRowsBeginningWith(const Ubiquitty::Terminal& terminal, std::u32string_view prefix)
{
    int found = 0;
    for (int index = 0; index < Ubiquitty::Screen::scrollbackCount(&terminal); ++index) {
        const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&terminal, index);
        bool matches = row && row->length >= static_cast<int>(prefix.size());
        for (std::size_t column = 0; matches && column < prefix.size(); ++column)
            matches = row->cells[column].codePoint == prefix[column];
        found += matches ? 1 : 0;
    }
    return found;
}

// A line of `length` characters naming itself first, in letters that never
// repeat a row's worth at 80 columns.
std::string longLine(std::string_view name, std::size_t length)
{
    std::string line(name);
    while (line.size() < length)
        line.push_back(static_cast<char>('a' + line.size() % 23));
    return line;
}

Facts meteredFacts()
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.link = LinkCost::Metered;
    return facts;
}

void sendFacts(SyncLink& link, const Facts& facts)
{
    std::vector<std::uint8_t> body;
    encode(body, facts);
    link.sendToHost(FrameKind::Facts, body);
}

ClientPolicy keepingHistory(std::uint32_t lines)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.historyCacheLines = lines;
    return policy;
}

} // namespace

// A resize is a geometry change, which only a checkpoint says. The checkpoint
// names the history's address rather than carrying the whole ring, so the
// lines stay where they are and a reflow forces no second transfer.
TEST(aResizeSendsNoHistoryBytes)
{
    SyncLink link(0, 24);
    for (int batch = 0; batch < 10; ++batch) {
        link.write(lines(batch * 500, 500));
        link.step();
    }
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
    const std::uint64_t mark = link.host().counters().historyBytesSent;
    const std::uint64_t checkpoints = link.host().counters().checkpointsSent;

    for (const auto& [rows, columns] : { std::pair { 30, 100 }, std::pair { 24, 53 }, std::pair { 24, 80 } }) {
        link.resize(rows, columns);
        link.stepUntilQuiet();
        CHECK(link.host().counters().lastCheckpointBodyBytes < 16'384);
    }
    CHECK_EQ(link.host().counters().historyBytesSent - mark, std::uint64_t { 0 });
    CHECK_EQ(link.host().counters().checkpointsSent - checkpoints, std::uint64_t { 3 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// After every update a client applies on the stream, it holds every line
// below the head the update names: the appends went first.
TEST(appendsPrecedeTheUpdateThatScrollsThem)
{
    SyncLink link(0, 24);
    int written = 0;
    for (int step = 0; step < 120; ++step) {
        const int count = step % 17 == 0 ? 900 : step % 5;
        link.write(lines(written, count));
        written += count;
        link.step();
        const HistoryCache& cache = cacheOf(link);
        CHECK_MSG(cache.contiguousHead() >= cache.head(),
            ("step " + std::to_string(step) + ": the client holds through " + std::to_string(cache.contiguousHead()) + " under a head of " + std::to_string(cache.head())).c_str());
    }
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A taller screen pulls lines back and a shrink pushes the same lines out:
// neither direction sends a history byte.
TEST(aGrowThenShrinkSendsNoHistoryBytes)
{
    SyncLink link(0, 24);
    link.write(lines(0, 200));
    link.stepUntilQuiet();
    const std::uint64_t mark = link.host().counters().historyBytesSent;
    const std::uint64_t head = hostAddress(link).head;
    link.resize(34, 80);
    link.stepUntilQuiet();
    CHECK_EQ(hostAddress(link).head, head - 10);
    CHECK_EQ(cacheOf(link).head(), head - 10);
    link.resize(24, 80);
    link.stepUntilQuiet();
    CHECK_EQ(hostAddress(link).head, head);
    CHECK_EQ(link.host().counters().historyBytesSent - mark, std::uint64_t { 0 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A line pulled back onto the screen and edited there is another line when a
// shrink pushes it out again, and is sent again; its unedited neighbour is not.
TEST(aLineEditedWhilePulledBackIsSentAgain)
{
    SyncLink link(0, 24);
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    const std::uint64_t head = hostAddress(link).head;
    link.resize(26, 80);
    link.stepUntilQuiet();
    CHECK_EQ(hostAddress(link).head, head - 2);
    const std::uint64_t linesBefore = link.host().counters().historyLinesSent;
    link.write("\x1b[1;1Hedited\x1b[26;1H");
    link.stepUntilQuiet();
    link.resize(24, 80);
    link.stepUntilQuiet();
    CHECK_EQ(hostAddress(link).head, head);
    CHECK_EQ(link.host().counters().historyLinesSent - linesBefore, std::uint64_t { 1 });
    const DecodedLine* edited = cacheOf(link).line(head - 2);
    REQUIRE(edited != nullptr);
    CHECK(logicalLineHash(link.hostTerminal(), head - 2) == edited->lineHash);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A line whose only change is a proportional run's text, rewritten while it
// was pulled back, is another line: the client must not keep showing,
// copying or searching the old words.
TEST(aLineWhoseRunTextAloneWasRewrittenIsSentAgain)
{
    SyncLink link(0, 24);
    link.drawSizedText();
    link.write(lines(0, 100));
    link.stepUntilQuiet();
    const std::uint64_t head = hostAddress(link).head;
    const auto rewriteTopRow = [&](std::string_view text) {
        link.resize(26, 80);
        link.writeUnpublished("\x1b[1;1HR \x1b]66;p=1:c=6;" + std::string(text) + "\x1b\\\x1b[K\x1b[26;1H");
        link.resize(24, 80);
        link.stepUntilQuiet();
        REQUIRE(hostAddress(link).head == head);
    };
    const auto clientRunText = [&]() -> std::string {
        const DecodedLine* line = cacheOf(link).line(head - 2);
        CHECK(line != nullptr);
        if (!line)
            return {};
        return line->runTexts.empty() ? std::string {} : std::string(line->runTexts.front().codePoints.begin(), line->runTexts.front().codePoints.end());
    };
    rewriteTopRow("first");
    CHECK_STR_EQ(clientRunText(), "first");
    const std::uint64_t linesBefore = link.host().counters().historyLinesSent;
    rewriteTopRow("fixed");
    CHECK_EQ(link.host().counters().historyLinesSent - linesBefore, std::uint64_t { 1 });
    CHECK_STR_EQ(clientRunText(), "fixed");
    // The client's scrollback shows the new words, which it never saw on its
    // screen: the line was rewritten and pushed out between two frames.
    const Ubiquitty::Terminal& client = link.clientTerminal();
    bool isShown = false;
    for (int index = 0; index < Ubiquitty::Screen::scrollbackCount(&client); ++index) {
        const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&client, index);
        if (row && row->length > 2) {
            const std::span<const std::uint32_t> shown = Ubiquitty::runTextCodePoints(&client, row->cells[2]);
            isShown = isShown || std::string(shown.begin(), shown.end()) == "fixed";
        }
    }
    CHECK(isShown);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// The open line too: one whose only change is a run's text is replaced in the
// client's history, and the client's scrollback shows the new words.
TEST(anOpenLineWhoseRunTextAloneWasRewrittenIsReplaced)
{
    SyncLink link(0, 24);
    link.drawSizedText();
    link.write(lines(0, 100));
    link.write("R \x1b]66;p=1:c=6;first\x1b\\" + std::string(80, 'x') + "\r\n");
    link.write(lines(100, 22));
    link.stepUntilQuiet();
    REQUIRE(openLineHash(link.hostTerminal()).has_value());
    const auto clientOpenText = [&]() -> std::string {
        const DecodedLine* open = cacheOf(link).openLine();
        if (!open || open->runTexts.empty())
            return {};
        return std::string(open->runTexts.front().codePoints.begin(), open->runTexts.front().codePoints.end());
    };
    CHECK_STR_EQ(clientOpenText(), "first");
    // Pulled back, rewritten and pushed out again between two frames: the
    // open line the host next holds differs from the one it sent only in its
    // run's words.
    link.resize(25, 80);
    REQUIRE(!openLineHash(link.hostTerminal()).has_value());
    link.writeUnpublished("\x1b[1;1HR \x1b]66;p=1:c=6;fixed\x1b\\\x1b[25;1H");
    link.resize(24, 80);
    link.stepUntilQuiet();
    REQUIRE(openLineHash(link.hostTerminal()).has_value());
    CHECK_STR_EQ(clientOpenText(), "fixed");
    // The client's scrollback shows it: the open line's row is its newest.
    const Ubiquitty::Terminal& client = link.clientTerminal();
    const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&client, Ubiquitty::Screen::scrollbackCount(&client) - 1);
    REQUIRE(row != nullptr && row->length > 2);
    const std::span<const std::uint32_t> shown = Ubiquitty::runTextCodePoints(&client, row->cells[2]);
    CHECK_STR_EQ(std::string(shown.begin(), shown.end()), "fixed");
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// Proportional runs placed and scrolled into history between two frames reach
// the mirror with their whole text. The mirror never saw them on its screen,
// so its scrollback holds only what the history plane carried: each row must
// show its own run's words, not an anchor's first letter or another run's.
TEST(runsThatScrollOffBetweenFramesKeepTheirTextOnTheMirror)
{
    SyncLink link(0, 24);
    link.drawSizedText();
    link.write(lines(0, 30));
    link.stepUntilQuiet();
    link.writeUnpublished("A \x1b]66;p=1:c=6;alpha\x1b\\\r\nB \x1b]66;p=1:c=6;bravo\x1b\\\r\n" + lines(30, 40));
    // The next frame is the first that holds the screen after them.
    link.write("");
    link.stepUntilQuiet();
    const Ubiquitty::Terminal& client = link.clientTerminal();
    std::vector<std::string> shown;
    for (int index = 0; index < Ubiquitty::Screen::scrollbackCount(&client); ++index) {
        const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&client, index);
        if (!row || row->length <= 2)
            continue;
        const std::span<const std::uint32_t> text = Ubiquitty::runTextCodePoints(&client, row->cells[2]);
        if (!text.empty())
            shown.emplace_back(text.begin(), text.end());
    }
    REQUIRE(shown.size() == 2);
    CHECK_STR_EQ(shown[0], "alpha");
    CHECK_STR_EQ(shown[1], "bravo");
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

TEST(aFloodBeyondOneFrameOfAppendsTravelsAsAHistoryHeadTransfer)
{
    SyncLink link(0, 24);
    std::size_t heads = 0;
    std::size_t inlines = 0;
    link.onHostFrame([&](FrameKind kind, std::uint32_t, SyncLink::Carrier, std::size_t) {
        heads += kind == FrameKind::HistoryHead ? 1 : 0;
        inlines += kind == FrameKind::HistoryInline ? 1 : 0;
    });
    link.write(lines(0, 3'000, 70));
    link.stepUntilQuiet();
    CHECK(heads > 1);
    CHECK_EQ(inlines, std::size_t { 0 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");

    // A line or two fits one frame.
    heads = 0;
    inlines = 0;
    link.write(lines(3'000, 2));
    link.stepUntilQuiet();
    CHECK_EQ(heads, std::size_t { 0 });
    CHECK_EQ(inlines, std::size_t { 1 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// An unmetered client is backfilled newest first: the lines nearest the
// screen arrive first, and nothing below the depth arrives at all.
TEST(anUnmeteredAttachBackfillsNewestFirstToTheDepth)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.backfillDepthLines = 600;
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 750));
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane));
    REQUIRE(address.head - address.oldest > 600);
    for (int step = 0; step < 200 && (!link.client().historyCache(pane) || link.client().historyCache(pane)->lineCount() == 0); ++step)
        link.step();
    const HistoryCache& cache = *link.client().historyCache(pane);
    REQUIRE(cache.lineCount() > 0);
    CHECK((cache.cachedRanges() == std::vector<LineRange> { { address.head - 256, address.head } }));
    link.stepUntilQuiet();
    CHECK((cache.cachedRanges() == std::vector<LineRange> { { address.head - 600, address.head } }));
    CHECK_STR_EQ(link.disagreement(pane, 600), "");
}

// A datagram can be lost; history never rides one, so a lost update costs the
// next update and never a line.
TEST(historyNeverTravelsAsADatagram)
{
    SyncLink link(UbiquittyTest::syncMaxDatagramBytes, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane(24, 80, "$ ");
    std::size_t historyDatagrams = 0;
    std::size_t datagrams = 0;
    link.onHostFrame([&](FrameKind kind, std::uint32_t, SyncLink::Carrier carrier, std::size_t) {
        if (carrier != SyncLink::Carrier::Datagram)
            return;
        ++datagrams;
        historyDatagrams += kind == FrameKind::HistoryInline || kind == FrameKind::HistoryHead || kind == FrameKind::HistoryReset ? 1 : 0;
    });
    link.setLossPermille(100);
    for (int step = 0; step < 300; ++step) {
        link.write(lines(step * 3, 3));
        link.step();
    }
    link.setLossPermille(0);
    link.stepUntilQuiet();
    CHECK(datagrams > 50);
    CHECK_EQ(historyDatagrams, std::size_t { 0 });
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// An update on a datagram overtakes the stream that carries its appends: the
// client takes the head and holds a gap below it, which the stream fills.
TEST(aDatagramUpdateAheadOfItsAppendsLeavesAGapTheStreamFills)
{
    SyncLink link(UbiquittyTest::syncMaxDatagramBytes, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane(24, 80, lines(0, 40));
    link.stepUntilQuiet();
    const std::uint64_t supersetsBefore = link.host().counters().supersetUpdatesSent;
    link.holdDownstream();
    link.write(lines(40, 2));
    link.step();
    REQUIRE(link.host().counters().supersetUpdatesSent > supersetsBefore);
    const HistoryCache& cache = cacheOf(link);
    CHECK_EQ(cache.head(), hostAddress(link).head);
    CHECK(cache.contiguousHead() < cache.head());
    link.releaseDownstream();
    link.stepUntilQuiet();
    CHECK_EQ(cache.contiguousHead(), cache.head());
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

TEST(historyAckedReportsTheContiguousHead)
{
    SyncLink link(0, 24);
    link.write(lines(0, 300));
    link.stepUntilQuiet();
    CHECK_EQ(cacheOf(link).contiguousHead(), hostAddress(link).head);
    CHECK(link.host().historyAcked(link.handle()) == hostAddress(link).head);
}

TEST(aRecordsOnlyPaneReceivesNoRequestedHistory)
{
    SyncLink link(0, 24);
    link.write(lines(0, 300));
    link.stepUntilQuiet();
    const auto address = hostAddress(link);
    REQUIRE(address.head > address.oldest);
    link.client().setView(link.handle(), ViewState::RecordsOnly);
    link.stepUntilQuiet();
    const std::uint64_t before = link.host().counters().historyBytesSent;
    for (const HistoryPriority priority : { HistoryPriority::Viewport, HistoryPriority::Search, HistoryPriority::Backfill }) {
        std::vector<std::uint8_t> body;
        encode(body, HistoryWant { .historyEpoch = address.epoch, .ranges = { { address.oldest, address.head } }, .priority = priority });
        link.sendToHost(FrameKind::HistoryWant, body, link.handle());
        for (int step = 0; step < 20; ++step)
            link.step();
        CHECK_EQ(link.host().counters().historyBytesSent, before);
        CHECK(link.host().state() == ConnectionState::Attached);
    }
    link.client().setView(link.handle(), ViewState::Live);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

TEST(enteringRecordsOnlyWithdrawsInlineHistoryAndScreenForOnlyThatPane)
{
    SyncLink link(0, 24);
    const std::uint32_t other = link.addPane();
    link.write(lines(0, 30));
    link.stepUntilQuiet();
    const std::uint64_t historyBefore = link.host().counters().historyBytesSent;
    const std::uint64_t updatesBefore = link.host().counters().chainUpdatesSent;
    link.write(lines(30, 3));
    link.write(other, "other pane changes");
    link.serviceWithoutCarrying();
    REQUIRE(link.host().counters().historyBytesSent > historyBefore);
    REQUIRE(link.host().counters().chainUpdatesSent >= updatesBefore + 2);
    std::size_t hiddenFrames = 0;
    std::size_t otherUpdates = 0;
    link.onHostFrame([&](FrameKind kind, std::uint32_t handle, SyncLink::Carrier, std::size_t) {
        if (handle == link.handle() && (kind == FrameKind::HistoryInline || kind == FrameKind::Update || kind == FrameKind::CheckpointHead || kind == FrameKind::PaneDirty))
            ++hiddenFrames;
        if (handle == other && kind == FrameKind::Update)
            ++otherUpdates;
    });
    link.client().setView(link.handle(), ViewState::RecordsOnly);
    link.host().receiveStream(link.client().takeStreamOutput(), link.now());
    for (int step = 0; step < 20; ++step)
        link.step();
    CHECK_EQ(hiddenFrames, 0u);
    CHECK_EQ(otherUpdates, 1u);
    CHECK_STR_EQ(link.disagreement(other), "");
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.client().setView(link.handle(), ViewState::Live);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

TEST(enteringRecordsOnlyAbandonsQueuedHistory)
{
    SyncLink link(0, 24);
    link.write(lines(0, 700, 70));
    link.stepUntilQuiet();
    const auto address = hostAddress(link);
    std::vector<std::uint8_t> body;
    encode(body, HistoryWant { .historyEpoch = address.epoch, .ranges = { { address.oldest, address.head } }, .priority = HistoryPriority::Viewport });
    link.sendToHost(FrameKind::HistoryWant, body, link.handle());
    const std::uint64_t before = link.host().counters().historyBytesSent;
    link.serviceWithoutCarrying();
    REQUIRE(link.host().counters().historyBytesSent > before);
    std::size_t historyFrames = 0;
    link.onHostFrame([&](FrameKind kind, std::uint32_t, SyncLink::Carrier, std::size_t) {
        historyFrames += kind == FrameKind::HistoryHead || kind == FrameKind::HistoryInline || kind == FrameKind::BulkChunk ? 1 : 0;
    });
    link.client().setView(link.handle(), ViewState::RecordsOnly);
    link.host().receiveStream(link.client().takeStreamOutput(), link.now());
    for (int step = 0; step < 20; ++step)
        link.step();
    CHECK_EQ(historyFrames, 0u);
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.client().setView(link.handle(), ViewState::Live);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// The cache's bound: an editor's client keeps no history and says so, and the
// host neither backfills nor appends to it.
TEST(anAttachKeepingNoHistoryReceivesNoHistoryBytes)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, keepingHistory(0));
    std::size_t historyFrames = 0;
    std::size_t resets = 0;
    link.onHostFrame([&](FrameKind kind, std::uint32_t, SyncLink::Carrier, std::size_t) {
        historyFrames += kind == FrameKind::HistoryInline || kind == FrameKind::HistoryHead ? 1 : 0;
        resets += kind == FrameKind::HistoryReset ? 1 : 0;
    });
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 750));
    link.stepUntilQuiet();
    for (int batch = 0; batch < 20; ++batch) {
        link.write(lines(750 + batch * 100, batch % 4 == 0 ? 900 : 100));
        link.step();
    }
    link.resize(30, 100);
    link.stepUntilQuiet();
    CHECK(hostAddress(link).head > 2'000);
    CHECK_EQ(historyFrames, std::size_t { 0 });
    CHECK_EQ(link.host().counters().historyBytesSent, std::uint64_t { 0 });
    CHECK_EQ(link.host().counters().historyLinesSent, std::uint64_t { 0 });
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(link.hostTerminal(pane), link.clientTerminal(pane)), "");

    // A clear is still told: the client's screen-side state of the history,
    // its epoch, moves with the host's.
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    CHECK_EQ(resets, std::size_t { 1 });
    CHECK_EQ(historyFrames, std::size_t { 0 });
    CHECK_EQ(cacheOf(link).key().epoch, hostAddress(link).epoch);
    CHECK_EQ(cacheOf(link).lineCount(), std::size_t { 0 });
}

// The backfill goes as deep as the host's depth or the client's cache,
// whichever is shallower: a line the client would drop is not sent.
TEST(aBackfillGoesNoDeeperThanTheClientKeeps)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, keepingHistory(300));
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 750));
    link.stepUntilQuiet();
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane));
    REQUIRE(address.head - address.oldest > 600);
    CHECK_EQ(link.host().counters().historyLinesSent, std::uint64_t { 300 });
    CHECK((link.client().historyCache(pane)->cachedRanges() == std::vector<LineRange> { { address.head - 300, address.head } }));
    CHECK_STR_EQ(link.disagreement(pane, 300), "");
}

// The open line: a line longer than the screen, written slowly, is in the
// client's history at every frame after its first row leaves the screen. One
// that reaches the client only when it ends leaves a user scrolling up to read
// it with nothing.
TEST(aLongLineWrittenSlowlyIsInClientHistoryAtEveryFrame)
{
    SyncLink link(0, 24);
    link.write(lines(0, 30));
    link.stepUntilQuiet();
    const std::string line = longLine("OPEN", 10'000);
    bool hasLeft = false;
    int framesShown = 0;
    int framesCurrent = 0;
    for (std::size_t written = 0; written < line.size(); written += 100) {
        link.write(std::string_view(line).substr(written, 100));
        link.step();
        // From the first frame the client holds with the line's first row
        // gone, every frame shows it once; a frame the client is current on
        // shows it as the host holds it.
        const bool isCurrent = link.held(link.handle()) == link.newest(link.handle());
        hasLeft = hasLeft || (isCurrent && Ubiquitty::Screen::openLineRows(link.hostTerminal()));
        if (!hasLeft)
            continue;
        ++framesShown;
        CHECK_EQ(historyRowsBeginningWith(link.clientTerminal(link.handle()), U"OPEN"), 1);
        if (!isCurrent)
            continue;
        ++framesCurrent;
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&link.clientTerminal(link.handle())), Ubiquitty::Screen::scrollbackCount(&link.hostTerminal()));
        CHECK_STR_EQ(link.disagreement(link.handle()), "");
    }
    // The line fills the screen at its 1,920th character.
    CHECK(framesShown > 75);
    CHECK(framesCurrent > 60);
    // A hot line sends at most one open line per update.
    CHECK(link.host().counters().historyOpenLinesSent <= link.host().counters().chainUpdatesSent + link.host().counters().supersetUpdatesSent);
}

// The completed line takes the open line's number and place: one copy, the
// open line gone.
TEST(theCompletedLineReplacesTheOpenLine)
{
    SyncLink link(0, 24);
    link.write(lines(0, 30));
    link.write(longLine("DONE", 3'000));
    link.stepUntilQuiet();
    const std::uint64_t number = hostAddress(link).head;
    REQUIRE(cacheOf(link).openLine() != nullptr);
    CHECK_EQ(cacheOf(link).openLine()->number, number);
    link.write("\r\n" + lines(30, 24));
    link.stepUntilQuiet();
    CHECK(cacheOf(link).openLine() == nullptr);
    REQUIRE(cacheOf(link).line(number) != nullptr);
    CHECK(logicalLineHash(link.hostTerminal(), number) == cacheOf(link).line(number)->lineHash);
    CHECK_EQ(historyRowsBeginningWith(link.clientTerminal(link.handle()), U"DONE"), 1);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&link.clientTerminal(link.handle())), Ubiquitty::Screen::scrollbackCount(&link.hostTerminal()));
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// A reflow rewraps the open line's rows and may move the seam through it; the
// client holds one copy at every width.
TEST(aReflowWhileALineIsOpenKeepsOneCopy)
{
    SyncLink link(0, 24);
    link.write(lines(0, 30));
    link.write(longLine("WRAP", 4'000));
    link.stepUntilQuiet();
    REQUIRE(cacheOf(link).openLine() != nullptr);
    for (const auto& [rows, columns] : { std::pair { 24, 53 }, std::pair { 30, 120 }, std::pair { 24, 80 } }) {
        link.resize(rows, columns);
        link.stepUntilQuiet();
        CHECK_EQ(historyRowsBeginningWith(link.clientTerminal(link.handle()), U"WRAP"), 1);
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&link.clientTerminal(link.handle())), Ubiquitty::Screen::scrollbackCount(&link.hostTerminal()));
        CHECK_STR_EQ(link.disagreement(link.handle()), "");
    }
}

// A clear while a line is open purges it with the rest; one that clears only
// the open line's rows, numbering nothing, withdraws it.
TEST(aClearWhileALineIsOpenPurgesIt)
{
    SyncLink link(0, 24);
    link.write(lines(0, 30));
    link.write(longLine("GONE", 3'000));
    link.stepUntilQuiet();
    REQUIRE(cacheOf(link).openLine() != nullptr);
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    CHECK(cacheOf(link).openLine() == nullptr);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&link.clientTerminal(link.handle())), 0);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");

    link.write(longLine("ONLY", 3'000));
    link.stepUntilQuiet();
    REQUIRE(cacheOf(link).openLine() != nullptr);
    const std::uint64_t resets = link.host().counters().historyResetsSent;
    link.write("\x1b[3J");
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().historyResetsSent, resets);
    CHECK(cacheOf(link).openLine() == nullptr);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(&link.clientTerminal(link.handle())), 0);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

// An attach to a pane whose screen holds still with a line open is sent the
// open line after its checkpoint, with no update to carry it.
TEST(anAttachToAStillPaneIsSentItsOpenLine)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane(24, 80, lines(0, 30) + longLine("STILL", 3'000));
    link.stepUntilQuiet();
    REQUIRE(link.client().historyCache(pane)->openLine() != nullptr);
    CHECK_EQ(historyRowsBeginningWith(link.clientTerminal(pane), U"STILL"), 1);
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// The metered link: an attach over a metered link is pushed the one viewport
// above the screen, not backfilled two thousand lines deep, which the user
// would pay for whether or not they scroll.
TEST(aMeteredLinkPushesNothingBeyondOneViewport)
{
    SyncLink link(0, meteredFacts());
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 5'000), 10'000);
    for (int step = 0; step < 600; ++step)
        link.step();
    CHECK_EQ(link.host().counters().historyLinesSent, std::uint64_t { 24 });
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
    CHECK((link.client().historyCache(pane)->cachedRanges() == std::vector<LineRange> { { head - 24, head } }));
}

// A flood on a metered link pushes, with each update, only the lines within
// one viewport of it; what scrolled further is a gap the client pulls.
TEST(aMeteredFloodSendsOnlyLinesWithinOneViewportOfTheirUpdate)
{
    SyncLink link(0, meteredFacts());
    const std::uint32_t pane = link.addPane(24, 80, "$ ", 10'000);
    for (int step = 0; step < 40; ++step) {
        const std::uint64_t updatesBefore = link.host().counters().chainUpdatesSent + link.host().counters().supersetUpdatesSent;
        const std::uint64_t linesBefore = link.host().counters().historyLinesSent;
        link.write(lines(step * 200, 200));
        link.step();
        const std::uint64_t updates = link.host().counters().chainUpdatesSent + link.host().counters().supersetUpdatesSent - updatesBefore;
        CHECK(link.host().counters().historyLinesSent - linesBefore <= 24 * std::max<std::uint64_t>(updates, 1));
    }
    link.stepUntilQuiet();
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
    const HistoryCache& cache = *link.client().historyCache(pane);
    CHECK(cache.gaps({ head - 24, head }).empty());
    CHECK(!cache.gaps({ head - 1'000, head - 24 }).empty());
}

// A link that turns metered while a backfill is in flight drops it.
TEST(aLinkTurningMeteredAbortsTheBackfill)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 3'000, 300), 50'000);
    for (int step = 0; step < 200 && link.host().counters().historyLinesSent == 0; ++step)
        link.step();
    REQUIRE(!link.host().wantedHistory(pane, HistoryPriority::Backfill).empty());
    link.holdDownstream();
    link.step();
    sendFacts(link, meteredFacts());
    CHECK(link.host().wantedHistory(pane, HistoryPriority::Backfill).empty());
    CHECK_EQ(link.host().historyTransfers(pane, HistoryPriority::Backfill), std::size_t { 0 });
    link.releaseDownstream();
    link.stepUntilQuiet();
    const std::uint64_t sent = link.host().counters().historyLinesSent;
    for (int step = 0; step < 100; ++step)
        link.step();
    CHECK_EQ(link.host().counters().historyLinesSent, sent);
    CHECK(link.host().counters().historyLinesSent < 2'000);
}

// A link that turns unmetered resumes the backfill to the client's depth from
// where the history stands.
TEST(aLinkTurningUnmeteredResumesTheBackfill)
{
    SyncLink link(0, meteredFacts());
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 3'000), 50'000);
    link.stepUntilQuiet();
    REQUIRE(link.host().counters().historyLinesSent == 24);
    sendFacts(link, UbiquittyTest::syncDefaultFacts());
    link.stepUntilQuiet();
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
    CHECK(link.client().historyCache(pane)->gaps({ head - defaultBackfillDepthLines, head }).empty());
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// A metered client pulls what it scrolls to, and a backfill want of its own
// is not the push it would make it.
TEST(aMeteredClientStillPullsWhatItScrollsTo)
{
    SyncLink link(0, meteredFacts());
    const std::uint32_t pane = link.bindPane(24, 80, lines(0, 3'000), 50'000);
    link.stepUntilQuiet();
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
    const LineRange viewport { head - 300, head - 276 };
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, std::vector<LineRange> { viewport }));
    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Backfill, std::vector<LineRange> { { head - 1'000, head - 300 } }));
    link.stepUntilQuiet();
    const HistoryCache& cache = *link.client().historyCache(pane);
    CHECK(cache.gaps(viewport).empty());
    for (std::uint64_t number = viewport.from; number < viewport.to; ++number)
        CHECK(logicalLineHash(link.hostTerminal(pane), number) == cache.line(number)->lineHash);
    CHECK((cache.gaps({ head - 1'000, head - 300 }) == std::vector<LineRange> { { head - 1'000, head - 300 } }));
}

// The fixture clock makes history wire costs deterministic. Set
// UBQ_SYNC_HISTORY_REPORT to print the report for an external measurement record.
TEST(theHistoryPlaneCostsWhatTheBaselineRecords)
{
    struct Row {
        std::string scenario;
        std::uint64_t checkpointBytes = 0;
        std::uint64_t historyBytes = 0;
        std::uint64_t historyLines = 0;
        std::size_t transfers = 0;
        std::uint64_t aborts = 0;
    };
    std::vector<Row> rows;
    const auto measure = [&](const std::string& scenario, SyncLink& link, const std::function<void()>& run) {
        std::size_t heads = 0;
        link.onHostFrame([&](FrameKind kind, std::uint32_t, SyncLink::Carrier, std::size_t) { heads += kind == FrameKind::HistoryHead ? 1 : 0; });
        const HostCounters before = link.host().counters();
        run();
        const HostCounters& after = link.host().counters();
        rows.push_back({ .scenario = scenario,
            .checkpointBytes = after.checkpointsSent > before.checkpointsSent ? after.lastCheckpointBodyBytes : 0,
            .historyBytes = after.historyBytesSent - before.historyBytesSent,
            .historyLines = after.historyLinesSent - before.historyLinesSent,
            .transfers = heads,
            .aborts = after.bulkAborts - before.bulkAborts });
    };

    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        const std::uint32_t pane = link.addPane(24, 80, lines(0, 5'000), 10'000);
        link.stepUntilQuiet();
        measure("three resizes, 30x100, 24x53, 24x80, over 5,000 lines", link, [&] {
            for (const auto& [height, width] : { std::pair { 30, 100 }, std::pair { 24, 53 }, std::pair { 24, 80 } }) {
                link.resize(height, width);
                link.stepUntilQuiet();
            }
        });
        CHECK_EQ(rows.back().historyBytes, std::uint64_t { 0 });
        (void)pane;
    }
    {
        SyncLink link(0, meteredFacts());
        measure("a metered attach over 5,000 lines", link, [&] {
            (void)link.bindPane(24, 80, lines(0, 5'000), 10'000);
            link.stepUntilQuiet();
        });
        CHECK_EQ(rows.back().historyLines, std::uint64_t { 24 });
    }
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        measure("an unmetered attach over 5,000 lines", link, [&] {
            (void)link.bindPane(24, 80, lines(0, 5'000), 10'000);
            link.stepUntilQuiet();
        });
        CHECK_EQ(rows.back().historyLines, std::uint64_t { defaultBackfillDepthLines });
    }
    {
        SyncLink link(0, meteredFacts());
        const std::uint32_t pane = link.bindPane(24, 80, lines(0, 5'000), 10'000);
        link.stepUntilQuiet();
        const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
        measure("1,000 viewport wants on a metered link, sent at once", link, [&] {
            for (std::uint64_t want = 0; want < 1'000; ++want) {
                const std::uint64_t to = head - 24 - 4 * want;
                REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, std::vector<LineRange> { { to - 24, to } }));
            }
            link.stepUntilQuiet();
        });
        CHECK_EQ(rows.back().transfers, std::size_t { 1 });
    }
    {
        // A flick over a slow link: a want a frame, for the lines of the
        // viewport the client does not hold, the viewport jumping each frame
        // so each want supersedes the last before it is through.
        SyncLink link(0, meteredFacts(), std::nullopt, ClientKind::Human, UbiquittyTest::LinkShape { .bytesPerSecond = 125'000, .oneWayDelayMicroseconds = 30'000 });
        const std::uint32_t pane = link.bindPane(24, 80, lines(0, 5'000, 300), 10'000);
        link.stepUntilQuiet();
        const std::uint64_t head = Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).head;
        const std::uint64_t held = head - Ubiquitty::Screen::historyAddress(link.hostTerminal(pane)).oldest;
        measure("1,000 viewport wants of 300-character lines, one a frame, over 1 Mbit/s and 60 ms", link, [&] {
            for (std::uint64_t want = 0; want < 1'000; ++want) {
                const std::uint64_t to = head - 24 - (want * 37) % (held - 48);
                const std::vector<LineRange> gaps = link.client().historyCache(pane)->gaps({ to - 24, to });
                if (!gaps.empty())
                    REQUIRE(link.client().wantHistory(pane, HistoryPriority::Viewport, gaps));
                link.step();
            }
            link.stepUntilQuiet();
        });
    }

    if (!std::getenv("UBQ_SYNC_HISTORY_REPORT"))
        return;
    std::printf("\n| scenario | checkpoint body B | history B | history lines | history transfers | BulkAborts |\n|---|---|---|---|---|---|\n");
    for (const Row& row : rows)
        std::printf("| %s | %llu | %llu | %llu | %zu | %llu |\n", row.scenario.c_str(), static_cast<unsigned long long>(row.checkpointBytes), static_cast<unsigned long long>(row.historyBytes),
            static_cast<unsigned long long>(row.historyLines), row.transfers, static_cast<unsigned long long>(row.aborts));
}

UBIQUITTY_TEST_MAIN()

// A line of box-drawing characters, each in a colour of its own: three bytes
// of text and a run per cell, so a long one's open line is too large for one
// frame and travels as a transfer.
std::string colouredLine(std::string_view name, std::size_t length)
{
    std::string line(name);
    for (std::size_t index = name.size(); index < length; ++index) {
        const char32_t box = U'\u2500' + static_cast<char32_t>(index % 64);
        line += "\x1b[38;5;" + std::to_string(index % 256) + "m";
        line += static_cast<char>(0xE0 | (box >> 12));
        line += static_cast<char>(0x80 | ((box >> 6) & 0x3F));
        line += static_cast<char>(0x80 | (box & 0x3F));
    }
    return line;
}

// Plan risk R10, on the reordering stub: a carrier whose lanes reorder hands
// the client a replaced open line's chunks after the abort that withdrew it
// and after its successor's head. The client never takes the old line over
// the new one: it ends holding one copy of the newest open line, and the
// connection stays. No carrier reorders yet — QUIC keeps one stream per client —
// so this pins the fence before per-lane streams exist.
TEST(aReplacedOpenLineNeverOverwritesItsSuccessorWhenLanesReorder)
{
    // A window wide enough for both lines' bodies to leave before the client
    // reads anything.
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.pacing.windowFloorBytes = 512 * 1024;
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    (void)link.addPane(24, 80, "$ ");
    link.write(lines(0, 30));
    link.stepUntilQuiet();
    link.holdDownstream();
    const std::uint64_t abortsBefore = link.host().counters().bulkAborts;
    link.write(colouredLine("RTEN", 16'000));
    for (int step = 0; step < 8; ++step)
        link.step();
    link.write(colouredLine("more", 300));
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK(link.host().counters().bulkAborts > abortsBefore);
    auto delivered = std::make_shared<std::vector<FrameKind>>();
    link.onHostFrame([delivered](FrameKind kind, std::uint32_t, Carrier, std::size_t) { delivered->push_back(kind); });
    link.releaseDownstreamReordered([](FrameKind kind, std::span<const std::uint8_t>) { return kind == FrameKind::BulkChunk; });
    // The reordering the case is about happened: two open lines' heads and the
    // abort between them went ahead of any chunk.
    const auto firstChunk = std::ranges::find(*delivered, FrameKind::BulkChunk);
    CHECK(std::count(delivered->begin(), firstChunk, FrameKind::HistoryHead) >= 2);
    CHECK(std::count(delivered->begin(), firstChunk, FrameKind::BulkAbort) >= 1);
    CHECK(firstChunk != delivered->end());
    link.stepUntilQuiet();
    CHECK(link.client().state() == ConnectionState::Attached);
    REQUIRE(cacheOf(link).openLine() != nullptr);
    REQUIRE(link.client().historyCache(link.handle()) != nullptr);
    CHECK_EQ(cacheOf(link).openLine()->number, hostAddress(link).head);
    CHECK_EQ(historyRowsBeginningWith(link.clientTerminal(link.handle()), U"RTEN"), 1);
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}
