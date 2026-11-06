// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_history_scan.cpp — the off-lock history scan (Core/Search/HistoryScan.h)
// finds what the synchronous history search finds, line by line and cell by
// cell, over raw and folded blocks and lines that cross them; it leaves the
// line still open on the screen, and a line whose start was evicted, to their
// owners, and defers a line naming the cluster pool to a scan with the
// terminal.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/LineStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Search/HistoryScan.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using Ubiquitty::LineStore;
using Ubiquitty::Search::HistoryHit;
using UbiquittyTest::feed;
namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;

namespace {

// The store asks its first reader for the gate's lock; a test reader brings a
// plain one.
class TestLock final : public Ubiquitty::PAL::PriorityInheritingLock {
public:
    void lock() override { m_mutex.lock(); }
    void unlock() override { m_mutex.unlock(); }

private:
    std::mutex m_mutex;
};

std::unique_ptr<Ubiquitty::Terminal> terminalOf(int rows, int columns, int scrollbackLines)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), scrollbackLines);
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

ScrollSearch::CompiledRegex compile(const std::string& pattern, Ubiquitty::CaseSensitivity sensitivity = Ubiquitty::CaseSensitivity::Insensitive)
{
    ScrollSearch::CompiledRegex compiled;
    char error[128] {};
    CHECK_MSG(ScrollSearch::compile(pattern.c_str(), sensitivity, &compiled, error), error);
    return compiled;
}

bool collect(void* context, const HistoryHit& hit)
{
    static_cast<std::vector<HistoryHit>*>(context)->push_back(hit);
    return true;
}

struct ScanOutcome {
    std::vector<HistoryHit> hits;
    std::vector<std::uint64_t> deferred;
};

// Completes a captured scan, copying facts when it asks for them.
ScanOutcome finishScan(Ubiquitty::Terminal& t, const ScrollSearch::CompiledRegex& compiled, LineStore::Snapshot& snapshot)
{
    ScanOutcome outcome;
    const auto form = ScrollSearch::matchingNormalizationForm(Ubiquitty::Unicode::NormalizationForm::None);
    Ubiquitty::Search::HistoryScan scanner;
    scanner.begin(snapshot, compiled, form);
    for (int steps = 0; steps < 1'000'000; ++steps) {
        const auto state = scanner.step(2, collect, &outcome.hits);
        if (state == Ubiquitty::Search::HistoryScanState::NeedsFacts) {
            const bool hasFacts = t.lineStore->captureRegionFacts(snapshot, scanner.pendingRegion());
            CHECK_MSG(hasFacts, "the facts the scan asked for were copied");
            if (!hasFacts)
                break;
            continue;
        }
        CHECK_MSG(state != Ubiquitty::Search::HistoryScanState::Stale, "nothing changed the store during the scan");
        if (state != Ubiquitty::Search::HistoryScanState::Scanning)
            break;
    }
    for (const std::uint64_t line : scanner.deferredLines()) {
        outcome.deferred.push_back(line);
        Ubiquitty::Search::scanHistoryLine(t, line, compiled, form, collect, &outcome.hits);
    }
    LineStore::releaseSnapshot(snapshot);
    std::ranges::sort(outcome.hits, [](const HistoryHit& a, const HistoryHit& b) { return std::tie(a.line, a.startCell) < std::tie(b.line, b.startCell); });
    return outcome;
}

// Runs a scan to completion the way a worker would, copying facts when the
// scan asks and scanning deferred lines with the terminal.
ScanOutcome scan(Ubiquitty::Terminal& t, const ScrollSearch::CompiledRegex& compiled, std::size_t startFacts = 64, std::optional<LineStore::SnapshotRange> range = std::nullopt)
{
    LineStore::Snapshot snapshot;
    snapshot.spareLock = std::make_unique<TestLock>();
    const bool isCaptured = t.lineStore && t.lineStore->captureSnapshot(snapshot, startFacts, range);
    CHECK_MSG(isCaptured, "the snapshot was captured");
    return isCaptured ? finishScan(t, compiled, snapshot) : ScanOutcome {};
}

std::vector<HistoryHit> searchHistory(const Ubiquitty::Terminal& t, const std::string& pattern, Ubiquitty::CaseSensitivity sensitivity = Ubiquitty::CaseSensitivity::Insensitive)
{
    const auto result = ScrollSearch::searchHistory(t, pattern, { 0, UINT64_MAX }, sensitivity, Ubiquitty::Unicode::NormalizationForm::None, 1u << 20);
    std::vector<HistoryHit> hits;
    if (!result)
        return hits;
    for (const auto& match : result->matches)
        hits.push_back({ .line = match.line, .startCell = match.startCell, .endCell = match.endCell });
    return hits;
}

std::string randomLine(std::mt19937& random, int index)
{
    const auto pick = [&random](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
    static const char* words[] = { "alpha", "beta", "Gamma", "delta", "needle", "NEEDLE", "x", "café", "naïve", "δέλτα", "\xE5\x9C\xB0\xE5\x9B\xBE" };
    std::string line = "L" + std::to_string(index) + ":";
    const int count = pick(0, 40);
    for (int word = 0; word < count; ++word) {
        line += " ";
        line += words[pick(0, static_cast<int>(sizeof(words) / sizeof(words[0])) - 1)];
    }
    return line;
}

void checkSameHits(const std::vector<HistoryHit>& scanned, const std::vector<HistoryHit>& oracle, const char* what)
{
    CHECK_MSG(scanned.size() == oracle.size(), what);
    const std::size_t count = std::min(scanned.size(), oracle.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (scanned[index] == oracle[index])
            continue;
        CHECK_MSG(false, what);
        return;
    }
}

} // namespace

TEST(theScanFindsWhatTheHistorySearchFinds)
{
    std::mt19937 random(5);
    auto t = terminalOf(6, 37, 20000);
    for (int index = 0; index < 3000; ++index)
        feed(*t, randomLine(random, index) + "\r\n");
    REQUIRE(Ubiquitty::Screen::scrollbackCount(t.get()) > 1000);
    for (const char* pattern : { "needle", "NEEDLE", "caf\xC3\xA9", "L1[0-9]*:", "beta gamma", "^L2", "x$", "\xE5\x9C\xB0" }) {
        checkSameHits(scan(*t, compile(pattern)).hits, searchHistory(*t, pattern), pattern);
        checkSameHits(scan(*t, compile(pattern, Ubiquitty::CaseSensitivity::Sensitive)).hits,
            searchHistory(*t, pattern, Ubiquitty::CaseSensitivity::Sensitive), pattern);
    }
}

TEST(theScanReadsFoldedBlocksAsTheyWereStored)
{
    std::mt19937 random(9);
    auto t = terminalOf(6, 50, 20000);
    for (int index = 0; index < 4000; ++index)
        feed(*t, randomLine(random, index) + "\r\n");
    while (t->lineStore->foldColdBlocks(64)) {
    }
    REQUIRE_MSG(t->lineStore->foldedBlockCount() > 0, "some blocks folded");
    for (const char* pattern : { "needle", "L3[0-9]+:", "\xCE\xB4\xCE\xAD\xCE\xBB\xCF\x84\xCE\xB1" })
        checkSameHits(scan(*t, compile(pattern)).hits, searchHistory(*t, pattern), pattern);
}

TEST(aRangedCaptureReadsOnlyItsCompletedLinesAndBlocks)
{
    for (const bool shouldFold : { false, true }) {
        auto terminal = terminalOf(4, 40, 10000);
        for (int line = 0; line < 2000; ++line)
            feed(*terminal, "line " + std::to_string(line) + " needle needle\r\n");
        if (shouldFold) {
            while (terminal->lineStore->foldColdBlocks(64)) {
            }
            REQUIRE(terminal->lineStore->foldedBlockCount() > 0);
        }
        const LineStore::SnapshotRange range { .firstLine = 600, .endLine = 610 };
        LineStore::Snapshot snapshot;
        snapshot.spareLock = std::make_unique<TestLock>();
        REQUIRE(terminal->lineStore->captureSnapshot(snapshot, 0, range));
        CHECK_EQ(snapshot.front.number, range.firstLine);
        CHECK_EQ(snapshot.headLine, range.endLine);
        CHECK(snapshot.regions.size() < static_cast<std::size_t>(terminal->lineStore->blockCount()));
        auto expected = searchHistory(*terminal, "needle");
        std::erase_if(expected, [&range](const HistoryHit& hit) { return hit.line < range.firstLine || hit.line >= range.endLine; });
        checkSameHits(finishScan(*terminal, compile("needle"), snapshot).hits, expected, "the captured interval matches the reference");
        CHECK(scan(*terminal, compile("needle"), 0, LineStore::SnapshotRange { 9000, 10000 }).hits.empty());
    }
}

TEST(evictionDuringFactCaptureDoesNotInventACompleteFrontLine)
{
    auto terminal = terminalOf(2, 10, 30);
    feed(*terminal, std::string(100, 'x') + "\r\n");
    for (int line = 0; line < 20; ++line)
        feed(*terminal, "quiet\r\n");
    LineStore::Snapshot snapshot;
    snapshot.spareLock = std::make_unique<TestLock>();
    REQUIRE(terminal->lineStore->captureSnapshot(snapshot, 0));
    REQUIRE(snapshot.front.continuity == LineStore::LineContinuity::StartsLine);
    REQUIRE(snapshot.regions.size() == 1);
    feed(*terminal, "quiet\r\nquiet\r\nquiet\r\n");
    REQUIRE(terminal->lineStore->frontLine().continuity == LineStore::LineContinuity::ContinuesLine);
    CHECK(finishScan(*terminal, compile("x"), snapshot).hits.empty());
}

// Lines thousands of cells long span several blocks; a match is still found
// once, in its line, across the block boundary it straddles.
TEST(aLineThatCrossesBlocksIsScannedWhole)
{
    auto t = terminalOf(6, 80, 20000);
    for (int index = 0; index < 12; ++index) {
        std::string line = "start" + std::to_string(index) + " ";
        while (line.size() < 4000)
            line += "filler ";
        line += "TAIL" + std::to_string(index);
        feed(*t, line + "\r\n");
    }
    for (const char* pattern : { "start[0-9]+", "TAIL[0-9]+", "filler TAIL", "r filler f" })
        checkSameHits(scan(*t, compile(pattern)).hits, searchHistory(*t, pattern), pattern);
}

// The line still being written wraps from history onto the screen; its stored
// rows are the screen search's, with the rest of the line.
TEST(theOpenLineIsLeftToTheScreen)
{
    auto t = terminalOf(4, 20, 1000);
    for (int index = 0; index < 10; ++index)
        feed(*t, "line " + std::to_string(index) + "\r\n");
    // Eight rows of a four-row screen: the first rows are stored.
    feed(*t, "open: ");
    for (int index = 0; index < 20; ++index)
        feed(*t, "needle ");
    REQUIRE(t->lineStore->openLine().has_value());
    const auto outcome = scan(*t, compile("needle"));
    CHECK_MSG(outcome.hits.empty(), "no hit in the open line's stored rows");
    CHECK(!scan(*t, compile("line 3")).hits.empty());
}

// A line whose first rows were evicted has lost the cells its matches would
// count from.
TEST(aLineWhoseStartWasEvictedIsNotScanned)
{
    auto t = terminalOf(4, 10, 30);
    std::string longLine = "first ";
    while (longLine.size() < 200)
        longLine += "needle ";
    feed(*t, longLine + "\r\n");
    // Twenty rows of the long line and twenty after it overflow the thirty
    // the history keeps by more than the screen holds.
    for (int index = 0; index < 20; ++index)
        feed(*t, "row" + std::to_string(index) + "\r\n");
    REQUIRE_MSG(t->lineStore->frontLine().continuity == LineStore::LineContinuity::ContinuesLine, "eviction cut the long line");
    const auto outcome = scan(*t, compile("needle"));
    CHECK_MSG(outcome.hits.empty(), "the cut line is not scanned");
    CHECK(!scan(*t, compile("row3")).hits.empty());
}

// A cell whose cluster lives in the pool can only be read with the terminal;
// its line waits for the lock and is then found as the oracle finds it.
TEST(aLineNamingTheClusterPoolIsDeferredAndFound)
{
    auto t = terminalOf(4, 40, 1000);
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    feed(*t, "plain needle\r\n");
    feed(*t, family + " needle\r\n");
    for (int index = 0; index < 6; ++index)
        feed(*t, "more\r\n");
    const auto outcome = scan(*t, compile("needle"));
    CHECK_MSG(!outcome.deferred.empty(), "the pooled cluster's line was deferred");
    checkSameHits(outcome.hits, searchHistory(*t, "needle"), "needle");
}

// A tab leaves a gap of unwritten cells; a match after it counts the written
// cells before it, as a logical position does.
TEST(aMatchAfterAGapCountsWrittenCells)
{
    auto t = terminalOf(4, 40, 1000);
    feed(*t, "ab\tcd\r\n");
    for (int index = 0; index < 6; ++index)
        feed(*t, "more\r\n");
    const auto outcome = scan(*t, compile("cd"));
    REQUIRE(outcome.hits.size() == 1);
    CHECK_EQ(outcome.hits[0].startCell, 2u);
    CHECK_EQ(outcome.hits[0].endCell, 4u);
}

UBIQUITTY_TEST_MAIN()
