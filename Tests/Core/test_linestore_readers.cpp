// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_linestore_readers — a reader on another thread reads the history
// without the model lock (LineStore::Snapshot, RegionRead).
//
// Pins: a snapshot covers every retained row once, in raw, folded and blank
// regions, and reads back what the store holds; a registered reader holds
// folding off; eviction under a reader's hazard hands the block to the reader
// rather than reusing it; a released block and a store changed by a rare
// mutation read as invalid; and, under a writer appending, evicting, folding
// and clearing at full speed on another thread, every row a reader validates
// is the row that was stored (run under ThreadSanitizer and AddressSanitizer on
// Linux, where a torn or freed read is a report rather than luck).

#include "Core/LineStore.h"
#include "Core/TerminalOperations.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using Ubiquitty::Cell;
using Ubiquitty::LineStore;

namespace {

// Every row says which ordinal it was stored at, in its length and its cells,
// so a reader can tell a row from any other without asking the store.
int lengthFor(std::uint64_t ordinal) { return static_cast<int>(ordinal % 97 == 0 ? 0 : 1 + ordinal % 71); }

// Every third row is stored as bytes (appendDefaultASCII), so a reader meets
// compact rows beside cell rows in the same blocks.
bool isCompactFor(std::uint64_t ordinal) { return ordinal % 3 == 0; }

std::uint32_t codePointFor(std::uint64_t ordinal, int column)
{
    const auto base = static_cast<std::uint32_t>((ordinal * 31 + static_cast<std::uint64_t>(column)) % 4000);
    return isCompactFor(ordinal) ? 0x20u + base % 95 : 0x4E00u + base;
}

bool isWrappedFor(std::uint64_t ordinal) { return ordinal % 5 == 2; }

void appendRow(LineStore& store)
{
    const std::uint64_t ordinal = store.nextOrdinal();
    const int length = lengthFor(ordinal);
    if (isCompactFor(ordinal)) {
        std::uint8_t bytes[128];
        for (int column = 0; column < length; ++column)
            bytes[column] = static_cast<std::uint8_t>(codePointFor(ordinal, column));
        store.appendDefaultASCII(std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(length)), length, 0, 0, isWrappedFor(ordinal), false);
        return;
    }
    Cell row[128];
    for (int column = 0; column < length; ++column) {
        row[column] = Cell {};
        row[column].codePoint = codePointFor(ordinal, column);
    }
    store.append(row, length, 0, 0, isWrappedFor(ordinal), false);
}

// What a reader saw of one region: rows checked, and rows that were not the
// row stored at their ordinal.
struct RegionReport {
    bool isValid = false;
    int rows = 0;
    int mismatches = 0;
};

bool rowMatches(std::uint64_t ordinal, const LineStore::RowFacts& facts, const Cell* cells)
{
    if (facts.length != lengthFor(ordinal) || facts.isWrapped != isWrappedFor(ordinal))
        return false;
    for (int column = 0; column < facts.length; ++column) {
        if (!cells || cells[column].codePoint != codePointFor(ordinal, column))
            return false;
    }
    return true;
}

RegionReport readRegion(const LineStore::Snapshot& snapshot, const LineStore::SnapshotRegion& region, std::vector<Cell>& scratch)
{
    RegionReport report;
    const LineStore::RegionRead read(snapshot, region);
    report.isValid = read.isValid();
    if (!report.isValid)
        return report;
    if (region.isFolded) {
        const bool isDecoded = LineStore::decodeFoldedRegion(region, scratch, region.firstOrdinal, region.endOrdinal,
            [&](std::uint64_t ordinal, const LineStore::RowFacts& facts, const Cell* cells) {
                ++report.rows;
                report.mismatches += rowMatches(ordinal, facts, cells) ? 0 : 1;
            });
        if (!isDecoded)
            ++report.mismatches;
        return report;
    }
    const Cell* cells = region.cells;
    for (std::uint64_t ordinal = region.firstOrdinal; ordinal < region.endOrdinal; ++ordinal) {
        const LineStore::RowFacts& facts = snapshot.facts[region.factsOffset + static_cast<std::size_t>(ordinal - region.firstOrdinal)];
        ++report.rows;
        const Cell* rowCells = cells;
        if (facts.isCompact && facts.length && cells && region.text) {
            // As the scan reads a compact row: from its bytes, into its own scratch.
            if (scratch.size() < facts.length)
                scratch.resize(facts.length);
            LineStore::expandCompactRow(region.text + (cells - region.cells), scratch.data(), facts.length);
            rowCells = scratch.data();
        }
        report.mismatches += rowMatches(ordinal, facts, rowCells) ? 0 : 1;
        if (cells)
            cells += facts.length;
    }
    return report;
}

std::vector<Cell> decodeScratch() { return std::vector<Cell>(8192); }

// The store asks its first reader for the gate's lock; a test reader brings a
// plain one, which is all a test needs from the platform's.
class TestLock final : public Ubiquitty::PAL::PriorityInheritingLock {
public:
    void lock() override { m_mutex.lock(); }
    void unlock() override { m_mutex.unlock(); }

private:
    std::mutex m_mutex;
};

bool capture(const LineStore& store, LineStore::Snapshot& snapshot, std::size_t maxStartFacts)
{
    if (!snapshot.spareLock)
        snapshot.spareLock = std::make_unique<TestLock>();
    return store.captureSnapshot(snapshot, maxStartFacts);
}

} // namespace

TEST(aSnapshotReadsACompactRowFromItsBytesAndAfterItsExpansion)
{
    LineStore store;
    store.init(400);
    for (int row = 0; row < 300; ++row)
        appendRow(store);
    const auto readEverything = [&](int& rows, int& compactRows, int& mismatches) {
        LineStore::Snapshot snapshot;
        CHECK(capture(store, snapshot, 1u << 20));
        std::vector<Cell> scratch = decodeScratch();
        for (const auto& region : snapshot.regions) {
            const RegionReport report = readRegion(snapshot, region, scratch);
            CHECK(report.isValid);
            rows += report.rows;
            mismatches += report.mismatches;
            if (region.sequence && !region.isFolded && region.hasFacts) {
                for (std::uint64_t ordinal = region.firstOrdinal; ordinal < region.endOrdinal; ++ordinal)
                    compactRows += snapshot.facts[region.factsOffset + static_cast<std::size_t>(ordinal - region.firstOrdinal)].isCompact;
            }
        }
    };
    int rows = 0;
    int compactRows = 0;
    int mismatches = 0;
    readEverything(rows, compactRows, mismatches);
    CHECK_EQ(rows, store.count());
    CHECK_EQ(mismatches, 0);
    CHECK_MSG(compactRows > 0, "no row was stored compact, so the bytes path went unread");
    // A read through the store expands every compact row in place; the next
    // snapshot sees them as cell rows and reads the same content.
    for (int index = 0; index < store.count(); ++index)
        (void)store.at(index);
    rows = compactRows = mismatches = 0;
    readEverything(rows, compactRows, mismatches);
    CHECK_EQ(rows, store.count());
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(compactRows, 0);
}

TEST(aSnapshotCoversEveryRowOnceAndReadsWhatTheStoreHolds)
{
    LineStore store;
    store.init(5000);
    for (int row = 0; row < 6000; ++row)
        appendRow(store);
    // Fold the cold blocks, so the snapshot has folded, raw and blank rows.
    while (store.foldColdBlocks(16)) {
    }
    CHECK(store.foldedBlockCount() > 0);

    LineStore::Snapshot snapshot;
    CHECK(capture(store, snapshot, 1u << 20));
    CHECK_EQ(snapshot.frontOrdinal, store.frontOrdinal());
    CHECK_EQ(snapshot.nextOrdinal, store.nextOrdinal());
    std::vector<Cell> scratch = decodeScratch();
    std::uint64_t expected = snapshot.frontOrdinal;
    int rows = 0;
    int mismatches = 0;
    for (const auto& region : snapshot.regions) {
        CHECK_EQ(region.firstOrdinal, expected);
        expected = region.endOrdinal;
        const RegionReport report = readRegion(snapshot, region, scratch);
        CHECK(report.isValid);
        rows += report.rows;
        mismatches += report.mismatches;
    }
    CHECK_EQ(expected, snapshot.nextOrdinal);
    CHECK_EQ(rows, store.count());
    CHECK_EQ(mismatches, 0);
}

TEST(aRawRegionLeftUncopiedCopiesItsFactsLater)
{
    LineStore store;
    store.init(5000);
    for (int row = 0; row < 4000; ++row)
        appendRow(store);
    LineStore::Snapshot snapshot;
    CHECK(capture(store, snapshot, 64));
    std::size_t uncopied = 0;
    for (std::size_t index = 0; index < snapshot.regions.size(); ++index) {
        if (snapshot.regions[index].hasFacts)
            continue;
        ++uncopied;
        CHECK(store.captureRegionFacts(snapshot, index));
    }
    CHECK_MSG(uncopied > 0, "a small start budget leaves the older regions for later");
    std::vector<Cell> scratch = decodeScratch();
    int mismatches = 0;
    for (const auto& region : snapshot.regions)
        mismatches += readRegion(snapshot, region, scratch).mismatches;
    CHECK_EQ(mismatches, 0);
}

TEST(aRegisteredReaderHoldsFoldingOff)
{
    LineStore store;
    store.init(5000);
    for (int row = 0; row < 6000; ++row)
        appendRow(store);
    LineStore::Snapshot snapshot;
    CHECK(capture(store, snapshot, 1u << 20));
    CHECK_MSG(store.foldColdBlocks(64), "the fold is still owed");
    CHECK_EQ(store.foldedBlockCount(), 0);
    LineStore::releaseSnapshot(snapshot);
    while (store.foldColdBlocks(64)) {
    }
    CHECK(store.foldedBlockCount() > 0);
}

TEST(evictionUnderAHazardLeavesTheBlockToTheReader)
{
    LineStore store;
    store.init(400);
    for (int row = 0; row < 400; ++row)
        appendRow(store);
    LineStore::Snapshot snapshot;
    CHECK(capture(store, snapshot, 1u << 20));
    // The front region is the oldest block's live rows.
    std::size_t front = 0;
    while (front < snapshot.regions.size() && !snapshot.regions[front].sequence)
        ++front;
    CHECK(front < snapshot.regions.size());
    const LineStore::SnapshotRegion region = snapshot.regions[front];
    std::vector<Cell> copy(region.cells, region.cells + 8);
    {
        const LineStore::RegionRead read(snapshot, region);
        CHECK(read.isValid());
        // Evict every row the block backs, and more: the store releases the
        // block and starts others while the reader still reads it.
        for (int row = 0; row < 2000; ++row)
            appendRow(store);
        CHECK(snapshot.gate->deferredSlab.load() != nullptr);
        bool isUnchanged = true;
        for (std::size_t index = 0; index < copy.size(); ++index)
            isUnchanged = isUnchanged && region.cells[index].codePoint == copy[index].codePoint;
        CHECK_MSG(isUnchanged, "the released block's cells are the reader's until its read ends");
    }
    CHECK_MSG(snapshot.gate->deferredSlab.load() == nullptr, "the reader frees the block as its read ends");
    const LineStore::RegionRead again(snapshot, region);
    CHECK_MSG(!again.isValid(), "a released block reads as invalid");
}

TEST(aRareMutationMakesTheSnapshotStale)
{
    LineStore store;
    store.init(1000);
    for (int row = 0; row < 800; ++row)
        appendRow(store);
    LineStore::Snapshot snapshot;
    CHECK(capture(store, snapshot, 1u << 20));
    CHECK(!snapshot.isStale());
    store.dropNewest(3);
    CHECK(snapshot.isStale());
    for (const auto& region : snapshot.regions) {
        if (!region.sequence)
            continue;
        const LineStore::RegionRead read(snapshot, region);
        CHECK(!read.isValid());
    }
    CHECK(capture(store, snapshot, 1u << 20));
    CHECK(!snapshot.isStale());
    store.clear();
    CHECK(snapshot.isStale());
}

TEST(aSnapshotOutlivesItsStore)
{
    auto store = std::make_unique<LineStore>();
    store->init(1000);
    for (int row = 0; row < 900; ++row)
        appendRow(*store);
    LineStore::Snapshot snapshot;
    CHECK(capture(*store, snapshot, 1u << 20));
    store.reset();
    CHECK(snapshot.isStale());
    for (const auto& region : snapshot.regions) {
        const LineStore::RegionRead read(snapshot, region);
        CHECK(!read.isValid() || !region.sequence);
    }
}

// A writer at full speed and a reader on another thread. The writer holds the
// model lock for each batch it stores, and folds and clears now and then,
// which are the mutations that rewrite or free history beside eviction. The
// reader captures under the lock and reads without it, and every row it
// validates must be the row stored at its ordinal.
TEST(aReaderBesideAFloodOnlyEverReadsStoredRows)
{
    LineStore store;
    store.init(3000);
    std::mutex modelLock;
    std::atomic<bool> isDone { false };
    std::thread writer([&] {
        for (int batch = 0; batch < 6000; ++batch) {
            const std::scoped_lock hold(modelLock);
            for (int row = 0; row < 32; ++row)
                appendRow(store);
            if (batch % 97 == 0)
                (void)store.foldColdBlocks(2);
            if (batch % 1009 == 0)
                store.dropNewest(7);
            if (batch % 2503 == 0)
                store.clear();
        }
        isDone.store(true);
    });

    LineStore::Snapshot snapshot;
    std::vector<Cell> scratch = decodeScratch();
    int validRegions = 0;
    int rows = 0;
    int mismatches = 0;
    while (!isDone.load()) {
        {
            const std::scoped_lock hold(modelLock);
            if (!capture(store, snapshot, 1u << 14))
                continue;
        }
        for (std::size_t index = 0; index < snapshot.regions.size(); ++index) {
            if (!snapshot.regions[index].isFolded && !snapshot.regions[index].hasFacts) {
                const std::scoped_lock hold(modelLock);
                if (!store.captureRegionFacts(snapshot, index))
                    continue;
            }
            const RegionReport report = readRegion(snapshot, snapshot.regions[index], scratch);
            if (!report.isValid)
                continue;
            ++validRegions;
            rows += report.rows;
            mismatches += report.mismatches;
        }
    }
    writer.join();
    LineStore::releaseSnapshot(snapshot);
    CHECK_MSG(validRegions > 0, "the reader read something while the writer ran");
    CHECK_MSG(rows > 0, "and checked rows");
    CHECK_EQ(mismatches, 0);
}

UBIQUITTY_TEST_MAIN()
