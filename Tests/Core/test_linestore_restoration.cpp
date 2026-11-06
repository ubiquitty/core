// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/LineStore.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace {

using Ubiquitty::Cell;
using Ubiquitty::LineStore;
using Ubiquitty::StoredLine;
using namespace Ubiquitty::Protocol::Sync;

struct AllocationObservation {
    std::size_t refuseAt = 0;
    std::size_t attempts = 0;
    std::size_t requestedBytes = 0;
    std::size_t refusals = 0;
};

// Only this binary's synchronous nothrow calls are observed. Grants preserve
// the ordinary new/delete pair; the observer itself owns no allocated log.
class ScopedAllocationObservation {
public:
    explicit ScopedAllocationObservation(AllocationObservation& observation)
        : m_previous(active().exchange(&observation))
    {
    }
    ~ScopedAllocationObservation() { active().store(m_previous); }
    ScopedAllocationObservation(const ScopedAllocationObservation&) = delete;
    ScopedAllocationObservation& operator=(const ScopedAllocationObservation&) = delete;

    static bool refuses(std::size_t bytes)
    {
        auto* observation = active().load();
        if (!observation)
            return false;
        ++observation->attempts;
        observation->requestedBytes += bytes;
        if (observation->attempts != observation->refuseAt)
            return false;
        ++observation->refusals;
        return true;
    }

private:
    static std::atomic<AllocationObservation*>& active()
    {
        static std::atomic<AllocationObservation*> observation { nullptr };
        return observation;
    }
    AllocationObservation* m_previous;
};

class Rows {
public:
    explicit Rows(std::span<const int> lengths, bool isOpen = false)
    {
        m_cells.reserve(lengths.size());
        m_rows.reserve(lengths.size());
        for (std::size_t index = 0; index < lengths.size(); ++index) {
            auto& cells = m_cells.emplace_back(static_cast<std::size_t>(lengths[index]), Ubiquitty::defaultBlankCell);
            for (std::size_t column = 0; column < cells.size(); ++column) {
                cells[column].codePoint = static_cast<std::uint32_t>('A' + (index + column) % 26);
                cells[column].setLinkID(7);
                cells[column].setClusterID(3);
                cells[column].combining[0] = 0x301;
                cells[column].setCombiningLength(1);
            }
            m_rows.push_back({ cells.empty() ? nullptr : cells.data(), lengths[index], isOpen || index % 3 != 2,
                static_cast<std::uint8_t>(index % 4), static_cast<std::uint8_t>(index % 8), index % 2 == 0, false });
        }
    }

    [[nodiscard]] std::span<const StoredLine> rows() const { return m_rows; }
    [[nodiscard]] LineStore::Continuation continuation() const
    {
        std::uint64_t completed = 0;
        for (const auto& row : m_rows)
            completed += !row.wrapped;
        return { .front = { 100, LineStore::LineContinuity::ContinuesLine }, .head = 100 + completed, .maxHead = 1000 + completed, .totalAppended = static_cast<long long>(m_rows.size()) + 500 };
    }

private:
    std::vector<std::vector<Cell>> m_cells;
    std::vector<StoredLine> m_rows;
};

void checkRows(const LineStore& store, std::span<const StoredLine> rows)
{
    REQUIRE(store.count() == static_cast<int>(rows.size()));
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto* actual = store.at(static_cast<int>(index));
        REQUIRE(actual);
        const auto& expected = rows[index];
        REQUIRE(actual->length == expected.length);
        CHECK(actual->wrapped == expected.wrapped);
        CHECK(actual->lineAttribute == expected.lineAttribute);
        CHECK(actual->mark == expected.mark);
        CHECK(actual->needsShaping == expected.needsShaping);
        if (expected.length)
            CHECK(std::memcmp(actual->cells, expected.cells, static_cast<std::size_t>(expected.length) * sizeof(Cell)) == 0);
    }
}

void checkContinuation(const LineStore& store, const LineStore::Continuation& before)
{
    const auto actual = store.continuation();
    CHECK(actual.front.number == before.front.number);
    CHECK(actual.front.continuity == before.front.continuity);
    CHECK(actual.head == before.head);
    CHECK(actual.maxHead == before.maxHead);
    CHECK(actual.totalAppended == before.totalAppended);
}

std::array<int, 17> packedLengths()
{
    // 16,384 cells fit two slabs only if rows could be split. They cannot:
    // eight 1,000-cell rows fill each slab, and the final 384 needs a third.
    std::array<int, 17> result;
    result.fill(1000);
    result.back() = 384;
    return result;
}

constexpr HandoverHistoryLimits historyLimits {
    std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::uint32_t>::max(),
    std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint64_t>::max()
};

void installHistory(Ubiquitty::Terminal& terminal, std::unique_ptr<LineStore> store, int configured = 50000)
{
    terminal.lineStore = std::move(store);
    terminal.historyEpoch = 9;
    terminal.historyEpochBase = 80;
    terminal.scrollbackLimit = configured;
    terminal.scrollbackCapacity = terminal.lineStore->capacity();
    terminal.scrollbackCount = terminal.lineStore->count();
}

} // namespace

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::refuses(bytes) ? nullptr : ::operator new(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::refuses(bytes) ? nullptr : ::operator new[](bytes);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(lineStoreRestorationMeasuresExactRowBoundaryOwnersBeforeAllocation)
{
    const Rows input(packedLengths());
    const auto continuation = input.continuation();
    LineStore::RestoreLayout layout(67, continuation);
    AllocationObservation measurement;
    {
        ScopedAllocationObservation scope(measurement);
        for (const auto& row : input.rows())
            REQUIRE(layout.noteRow(row.length, row.wrapped));
    }
    CHECK(measurement.attempts == 0);
    const auto plan = layout.plan();
    REQUIRE(plan);
    CHECK(plan->blocks == 3);
    CHECK(plan->cells == 16384);
    CHECK(plan->descriptorBytes == 67 * sizeof(StoredLine));
    CHECK(plan->indexBytes == (67 / 64 + 2) * sizeof(std::uint64_t));
    CHECK(plan->slabBytes == 3 * 8192 * sizeof(Cell));
    CHECK(plan->retainedBytes == sizeof(LineStore) + plan->descriptorBytes + plan->indexBytes + plan->blockDirectoryBytes + plan->slabBytes);
    AllocationObservation observation;
    auto restored = [&] {
        ScopedAllocationObservation scope(observation);
        return LineStore::tryRestore(67, input.rows(), continuation, plan->retainedBytes);
    }();
    REQUIRE(restored);
    CHECK(observation.attempts == 7); // object, descriptors, index, directory, three slabs
    CHECK(observation.requestedBytes == plan->retainedBytes);
    CHECK(observation.refusals == 0);
    CHECK((*restored)->capacity() == 67);
    CHECK((*restored)->ownedBackingBytes().value() + sizeof(LineStore) == plan->retainedBytes);
    checkRows(**restored, input.rows());
    checkContinuation(**restored, continuation);
}

TEST(lineStoreRestorationRefusesBudgetAndInvalidRowsBeforeAllocation)
{
    const Rows input(packedLengths());
    const auto continuation = input.continuation();
    const auto plan = LineStore::restorationPlan(67, input.rows(), continuation);
    REQUIRE(plan);
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        const auto refused = LineStore::tryRestore(67, input.rows(), continuation, plan->retainedBytes - 1);
        REQUIRE(!refused);
        CHECK(refused.error() == LineStore::StorageError::AllocationRefused);
    }
    CHECK(observation.attempts == 0);
    for (int fault = 0; fault < 8; ++fault) {
        auto rows = std::vector<StoredLine>(input.rows().begin(), input.rows().end());
        auto changed = continuation;
        int capacity = 67;
        switch (fault) {
        case 0:
            capacity = 0;
            break;
        case 1:
            capacity = 16;
            break;
        case 2:
            rows[0].length = Ubiquitty::maxColumns + 1;
            break;
        case 3:
            rows[0].cells = nullptr;
            break;
        case 4:
            changed.totalAppended = 1;
            break;
        case 5:
            ++changed.head;
            break;
        case 6:
            changed.maxHead = changed.head - 1;
            break;
        case 7:
            changed.front.continuity = static_cast<LineStore::LineContinuity>(2);
            break;
        }
        AllocationObservation invalid;
        {
            ScopedAllocationObservation scope(invalid);
            CHECK(!LineStore::tryRestore(capacity, rows, changed));
        }
        CHECK(invalid.attempts == 0);
    }
}

TEST(lineStoreRestorationRefusesEveryOwnedAllocationAndCanRetry)
{
    const Rows input(packedLengths());
    for (std::size_t ordinal = 1; ordinal <= 7; ++ordinal) {
        AllocationObservation observation { .refuseAt = ordinal };
        auto restored = [&] {
            ScopedAllocationObservation scope(observation);
            return LineStore::tryRestore(67, input.rows(), input.continuation());
        }();
        REQUIRE(!restored);
        CHECK(restored.error() == LineStore::StorageError::AllocationRefused);
        CHECK(observation.refusals == 1);
        CHECK(observation.attempts == ordinal);
    }
    const auto restored = LineStore::tryRestore(67, input.rows(), input.continuation());
    REQUIRE(restored);
    checkRows(**restored, input.rows());
}

TEST(lineStoreRestorationPreservesLargeOpenHistoryEmptyFrontAndCounterLimits)
{
    std::vector<int> lengths(12000, 1);
    lengths[33] = 0;
    lengths.back() = Ubiquitty::maxColumns;
    const Rows input(lengths, true);
    auto continuation = input.continuation();
    continuation.front.continuity = LineStore::LineContinuity::StartsLine;
    auto restored = LineStore::tryRestore(15001, input.rows(), continuation);
    REQUIRE(restored);
    CHECK((*restored)->capacity() == 15001);
    checkRows(**restored, input.rows());
    REQUIRE((*restored)->openLine());
    CHECK((*restored)->openLine()->rowCount == 12000);
    checkContinuation(**restored, continuation);
    Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'Z';
    (*restored)->append(&cell, 1, 0, 4, false, false);
    REQUIRE((*restored)->logicalLine(continuation.head));
    CHECK((*restored)->logicalLine(continuation.head)->rowCount == 12001);
    CHECK((*restored)->totalAppended() == continuation.totalAppended + 1);
    Ubiquitty::Terminal source;
    installHistory(source, std::move(*restored));
    const auto body = encodeHandoverHistory(source, historyLimits);
    REQUIRE(body);
    Ubiquitty::Terminal destination;
    REQUIRE(restoreHandoverHistory(body->view(), destination, historyLimits));
    CHECK(destination.scrollbackLimit == 50000);
    CHECK(destination.scrollbackCapacity == 15001);
    CHECK(destination.scrollbackCount == 12001);
    const auto reproduced = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(reproduced);
    CHECK(std::ranges::equal(body->view(), reproduced->view()));

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const LineStore::Continuation empty { .front = { maximum, LineStore::LineContinuity::ContinuesLine },
        .head = maximum,
        .maxHead = maximum,
        .totalAppended = std::numeric_limits<long long>::max() };
    auto emptyStore = LineStore::tryRestore(37, {}, empty);
    REQUIRE(emptyStore);
    checkContinuation(**emptyStore, empty);
    (*emptyStore)->append(&cell, 1, 0, 0, false, false);
    CHECK((*emptyStore)->count() == 0);
    checkContinuation(**emptyStore, empty);
}

TEST(lineStoreRestorationPreservesTheMaximumFrontThroughHistoryRoundtrip)
{
    const std::array<int, 3> lengths { 3, 0, 4 };
    const Rows input(lengths, true);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array<std::span<const StoredLine>, 2> rowSets { input.rows(), std::span<const StoredLine> {} };
    for (const auto continuity : { LineStore::LineContinuity::ContinuesLine, LineStore::LineContinuity::StartsLine }) {
        const LineStore::Continuation continuation { .front = { maximum, continuity },
            .head = maximum,
            .maxHead = maximum,
            .totalAppended = std::numeric_limits<long long>::max() };
        for (const auto rows : rowSets) {
            auto restored = LineStore::tryRestore(7, rows, continuation);
            REQUIRE(restored);
            checkRows(**restored, rows);
            checkContinuation(**restored, continuation);
            CHECK((*restored)->frontLineNumber() == maximum);
            CHECK(!(*restored)->logicalLine(0));
            CHECK(!(*restored)->logicalLine(maximum));
            const auto open = (*restored)->openLine();
            if (!rows.empty() && continuity == LineStore::LineContinuity::StartsLine) {
                REQUIRE(open);
                CHECK(open->firstIndex == 0);
                CHECK(open->rowCount == static_cast<int>(rows.size()));
            } else
                CHECK(!open);

            Ubiquitty::Terminal source;
            installHistory(source, std::move(*restored));
            const auto body = encodeHandoverHistory(source, historyLimits);
            REQUIRE(body);
            Ubiquitty::Terminal destination;
            REQUIRE(restoreHandoverHistory(body->view(), destination, historyLimits));
            REQUIRE(destination.lineStore);
            CHECK(destination.scrollbackCapacity == 7);
            CHECK(destination.scrollbackCount == static_cast<int>(rows.size()));
            CHECK(destination.lineStore->frontLineNumber() == maximum);
            checkRows(*destination.lineStore, rows);
            checkContinuation(*destination.lineStore, continuation);
            const auto reproduced = encodeHandoverHistory(destination, historyLimits);
            REQUIRE(reproduced);
            CHECK(std::ranges::equal(body->view(), reproduced->view()));
        }
    }
}

TEST(lineStoreAppendRefusesSlabAndDirectoryBeforeEvictingAnyHistory)
{
    std::array<int, 16> lengths;
    lengths.fill(Ubiquitty::maxColumns);
    const Rows input(lengths);
    const auto plan = LineStore::restorationPlan(16, input.rows(), input.continuation());
    REQUIRE(plan && plan->blocks == 2);
    auto restored = LineStore::tryRestore(16, input.rows(), input.continuation());
    REQUIRE(restored);
    const auto beforeBytes = (*restored)->ownedBackingBytes();
    REQUIRE(beforeBytes);
    const auto* first = (*restored)->at(0)->cells;
    const Cell cell = Ubiquitty::defaultBlankCell;
    for (std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 } }) {
        AllocationObservation observation { .refuseAt = ordinal };
        {
            ScopedAllocationObservation scope(observation);
            (*restored)->append(&cell, 1, 0, 0, false, false);
        }
        CHECK(observation.attempts == ordinal);
        CHECK(observation.refusals == 1);
        CHECK((*restored)->at(0)->cells == first);
        CHECK((*restored)->ownedBackingBytes() == beforeBytes);
        checkRows(**restored, input.rows());
        checkContinuation(**restored, input.continuation());
    }
    (*restored)->append(&cell, 1, 0, 0, false, false);
    CHECK((*restored)->count() == 16);
    CHECK((*restored)->totalAppended() == input.continuation().totalAppended + 1);
    CHECK((*restored)->blockCount() == 3);
    // Live directory growth admits four actual slots, not just three in use.
    CHECK((*restored)->ownedBackingBytes().value() == *beforeBytes + 8192 * sizeof(Cell) + plan->blockDirectoryBytes);
}

TEST(lineStoreAppendRecyclesTheExactSlabReleasedByItsOwnEviction)
{
    const std::array<int, 1> lengths { 1 };
    const Rows input(lengths);
    auto restored = LineStore::tryRestore(1, input.rows(), input.continuation());
    REQUIRE(restored);
    const auto* first = (*restored)->at(0)->cells;
    const auto bytes = (*restored)->ownedBackingBytes();
    Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'N';
    AllocationObservation observation { .refuseAt = 1 };
    {
        ScopedAllocationObservation scope(observation);
        for (int index = 0; index < 5; ++index)
            (*restored)->append(&cell, 1, 0, 0, false, false);
    }
    CHECK(observation.attempts == 0);
    CHECK((*restored)->at(0)->cells == first);
    CHECK((*restored)->at(0)->cells[0].codePoint == 'N');
    CHECK((*restored)->ownedBackingBytes() == bytes);
    CHECK((*restored)->totalAppended() == input.continuation().totalAppended + 5);
}

TEST(lineStoreOwnedCapacityIncludesParkedAndReusableDecodeSlabs)
{
    std::vector<int> lengths(300, 100);
    const Rows input(lengths);
    const auto plan = LineStore::restorationPlan(300, input.rows(), input.continuation());
    auto restored = LineStore::tryRestore(300, input.rows(), input.continuation());
    REQUIRE(plan && restored);
    (void)(*restored)->foldColdBlocks(20);
    REQUIRE((*restored)->foldedBlockCount() > 0);
    const auto beforeRead = (*restored)->ownedBackingBytes();
    REQUIRE(beforeRead);
    REQUIRE((*restored)->at(0));
    CHECK((*restored)->ownedBackingBytes().value() == *beforeRead + 8192 * sizeof(Cell));
    REQUIRE((*restored)->at(0));
    CHECK((*restored)->ownedBackingBytes().value() == *beforeRead + 8192 * sizeof(Cell));
    (*restored)->clear();
    CHECK((*restored)->ownedBackingBytes().value() == plan->descriptorBytes + plan->indexBytes + plan->blockDirectoryBytes);

    std::array<int, 16> fullLengths;
    fullLengths.fill(Ubiquitty::maxColumns);
    const Rows full(fullLengths);
    auto raw = LineStore::tryRestore(16, full.rows(), full.continuation());
    REQUIRE(raw);
    const auto beforeDrop = (*raw)->ownedBackingBytes();
    const auto beforeArena = (*raw)->arenaBytes();
    (*raw)->dropOldest(8);
    CHECK((*raw)->arenaBytes() + 8192 * sizeof(Cell) == beforeArena);
    CHECK((*raw)->ownedBackingBytes() == beforeDrop); // the retired slab is still owned
}

TEST(historyRestorationAdmitsExactOldStagingAndCandidateOverlap)
{
    const Rows input(packedLengths());
    auto owner = LineStore::tryRestore(67, input.rows(), input.continuation());
    REQUIRE(owner);
    Ubiquitty::Terminal source;
    installHistory(source, std::move(*owner));
    const auto body = encodeHandoverHistory(source, historyLimits);
    REQUIRE(body);
    std::vector<int> oldLengths(300, 100);
    const Rows oldRows(oldLengths);
    auto previous = LineStore::tryRestore(300, oldRows.rows(), oldRows.continuation());
    REQUIRE(previous);
    Ubiquitty::Terminal destination;
    installHistory(destination, std::move(*previous), 12345);
    UbiquittyTest::feed(destination, "unchanged screen");
    (void)destination.lineStore->foldColdBlocks(20);
    REQUIRE(destination.lineStore->at(0));
    const auto before = encodeHandoverHistory(destination, historyLimits);
    const auto oldBytes = destination.lineStore->ownedBackingBytes();
    REQUIRE(oldBytes && before);
    auto* const oldOwner = destination.lineStore.get();
    AllocationObservation measurement;
    const auto plan = [&] {
        ScopedAllocationObservation scope(measurement);
        return handoverHistoryStoragePlan(body->view(), destination, historyLimits);
    }();
    REQUIRE(plan);
    CHECK(measurement.attempts == 0);
    CHECK(plan->replacedBytes == *oldBytes + sizeof(LineStore));
    CHECK(plan->stagingBytes == 16384 * sizeof(Cell) + 17 * sizeof(StoredLine));
    CHECK(plan->peakPrepareBytes == plan->retainedCandidateBytes + plan->stagingBytes + plan->replacedBytes);
    AllocationObservation refused;
    {
        ScopedAllocationObservation scope(refused);
        const auto result = restoreHandoverHistory(body->view(), destination, historyLimits, plan->peakPrepareBytes - 1);
        REQUIRE(!result);
        CHECK(result.error() == HandoverBodyError::AllocationRefused);
    }
    CHECK(refused.attempts == 0);
    CHECK(destination.lineStore.get() == oldOwner);
    const auto afterRefusal = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(afterRefusal);
    CHECK(std::ranges::equal(before->view(), afterRefusal->view()));
    AllocationObservation accepted;
    {
        ScopedAllocationObservation scope(accepted);
        REQUIRE(restoreHandoverHistory(body->view(), destination, historyLimits, plan->peakPrepareBytes));
    }
    CHECK(accepted.requestedBytes + plan->replacedBytes == plan->peakPrepareBytes);
    CHECK(destination.scrollbackLimit == 50000);
    CHECK(destination.scrollbackCapacity == 67);
    checkRows(*destination.lineStore, input.rows());
    const auto reproduced = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(reproduced);
    CHECK(std::ranges::equal(body->view(), reproduced->view()));
    REQUIRE(UbiquittyTest::cell(destination, 0, 0));
    CHECK(UbiquittyTest::cell(destination, 0, 0)->codePoint == 'u');
}

TEST(historyRestorationEveryAllocationRefusalRetainsThePreviousOwnerAndCounters)
{
    const Rows input(packedLengths());
    auto owner = LineStore::tryRestore(67, input.rows(), input.continuation());
    REQUIRE(owner);
    Ubiquitty::Terminal source;
    installHistory(source, std::move(*owner));
    const auto body = encodeHandoverHistory(source, historyLimits);
    REQUIRE(body);
    const std::array<int, 2> lengths { 4, 0 };
    const Rows oldRows(lengths);
    auto previous = LineStore::tryRestore(7, oldRows.rows(), oldRows.continuation());
    REQUIRE(previous);
    Ubiquitty::Terminal destination;
    installHistory(destination, std::move(*previous), 99);
    const auto before = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(before);
    auto* const oldOwner = destination.lineStore.get();
    // Two staging arrays and the seven exact factory allocations above.
    for (std::size_t ordinal = 1; ordinal <= 9; ++ordinal) {
        AllocationObservation observation { .refuseAt = ordinal };
        {
            ScopedAllocationObservation scope(observation);
            const auto result = restoreHandoverHistory(body->view(), destination, historyLimits);
            REQUIRE(!result);
            CHECK(result.error() == HandoverBodyError::AllocationRefused);
        }
        CHECK(observation.attempts == ordinal);
        CHECK(observation.refusals == 1);
        CHECK(destination.lineStore.get() == oldOwner);
        CHECK(destination.scrollbackLimit == 99);
        CHECK(destination.scrollbackCapacity == 7);
        CHECK(destination.scrollbackCount == 2);
        const auto after = encodeHandoverHistory(destination, historyLimits);
        REQUIRE(after);
        CHECK(std::ranges::equal(before->view(), after->view()));
    }
    REQUIRE(restoreHandoverHistory(body->view(), destination, historyLimits));
    checkRows(*destination.lineStore, input.rows());
}

TEST(prependRefusedAllocationsLeaveRetainedHistoryIntact)
{
    for (int newestToDrop : { 0, 1 }) {
        std::size_t allocationCount = 0;
        for (std::size_t refusal = 0; refusal <= allocationCount; ++refusal) {
            LineStore store;
            store.init(newestToDrop ? 1 : 8);
            Cell retained;
            retained.codePoint = 'R';
            store.append(&retained, 1, 0, 0, false, false);
            Cell preceding;
            preceding.codePoint = 'P';
            const StoredLine row { &preceding, 1, false, 0, 0, false, false };
            AllocationObservation observation { .refuseAt = refusal };
            bool didPrepend = false;
            {
                ScopedAllocationObservation scoped(observation);
                didPrepend = store.prepend(std::span(&row, 1), { 0, LineStore::LineContinuity::StartsLine }, newestToDrop);
            }
            if (!refusal) {
                REQUIRE(didPrepend);
                allocationCount = observation.attempts;
                REQUIRE(allocationCount >= 4);
                CHECK(store.count() == 2 - newestToDrop);
            } else {
                CHECK(!didPrepend);
                CHECK(observation.refusals == 1);
                CHECK(store.count() == 1);
                CHECK(store.at(0)->cells[0].codePoint == 'R');
                CHECK(store.headLineNumber() == 1);
                CHECK(store.totalAppended() == 1);
            }
        }
    }
}

TEST(defaultASCIIAppendRefusesEveryAllocationBeforeEvictingHistory)
{
    for (const int initialRows : { 0, 8 }) {
        std::size_t allocationCount = 0;
        for (std::size_t refusal = 0; refusal <= allocationCount; ++refusal) {
            LineStore store;
            store.init(8);
            std::array<Cell, 1000> retained;
            retained.fill(Ubiquitty::defaultBlankCell);
            for (auto& cell : retained)
                cell.codePoint = 'R';
            for (int row = 0; row < initialRows; ++row)
                store.append(retained.data(), static_cast<int>(retained.size()), 0, 3, false, false);
            const auto before = store.continuation();
            std::array<std::uint8_t, 1000> text;
            text.fill('N');
            AllocationObservation observation { .refuseAt = refusal };
            {
                ScopedAllocationObservation scope(observation);
                store.appendDefaultASCII(text, static_cast<int>(text.size()), 0, 5, false, false);
            }
            if (!refusal) {
                allocationCount = observation.attempts;
                REQUIRE(allocationCount >= 1);
                CHECK_EQ(store.count(), initialRows ? initialRows : 1);
                CHECK_EQ(store.totalAppended(), initialRows + 1);
                text.fill('X');
                const auto* appended = store.at(store.count() - 1);
                REQUIRE(appended);
                CHECK_EQ(appended->mark, 5);
                for (int column = 0; column < appended->length; ++column)
                    CHECK_EQ(appended->cells[column].codePoint, 'N');
            } else {
                CHECK_EQ(observation.refusals, 1);
                CHECK_EQ(store.count(), initialRows);
                checkContinuation(store, before);
                for (int row = 0; row < initialRows; ++row) {
                    const auto* line = store.at(row);
                    REQUIRE(line);
                    CHECK_EQ(line->mark, 3);
                    CHECK_EQ(line->length, static_cast<int>(retained.size()));
                    CHECK(std::memcmp(line->cells, retained.data(), sizeof(retained)) == 0);
                }
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
