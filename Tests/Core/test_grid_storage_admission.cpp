// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/GridBounds.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace {

struct AllocationObservation {
    std::size_t calls = 0;
    std::size_t bytes = 0;
    std::array<std::size_t, 16> sizes {};
};

AllocationObservation* allocationObservation = nullptr;

/// Observes only the fallible backing allocations, without allocating a log.
class ObservingBackingAllocations {
public:
    explicit ObservingBackingAllocations(AllocationObservation& observation)
        : m_previous(allocationObservation)
    {
        allocationObservation = &observation;
    }
    ~ObservingBackingAllocations() { allocationObservation = m_previous; }

private:
    AllocationObservation* const m_previous;
};

void noteAllocation(std::size_t bytes)
{
    if (!allocationObservation)
        return;
    auto& observation = *allocationObservation;
    if (observation.calls < observation.sizes.size())
        observation.sizes[observation.calls] = bytes;
    ++observation.calls;
    observation.bytes += bytes;
}

std::size_t expectedBackingBytes(int rows, int columns)
{
    const auto rowCapacity = static_cast<std::size_t>(rows);
    const auto columnCapacity = static_cast<std::size_t>(columns);
    return 3 * rowCapacity * columnCapacity * sizeof(Ubiquitty::Cell)
        + 2 * rowCapacity * sizeof(Ubiquitty::Cell*)
        + 2 * rowCapacity * sizeof(Ubiquitty::RowState)
        + rowCapacity * ((columnCapacity + 7) / 8)
        + rowCapacity * sizeof(bool)
        + rowCapacity * sizeof(std::uint32_t);
}

Ubiquitty::Cell markedCell(std::uint32_t codePoint)
{
    auto cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = codePoint;
    return cell;
}

} // namespace

// Preserve the normal allocation/deallocation pair. The observation contains
// no allocator of its own, and production backing arrays use these two forms.
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    noteAllocation(bytes);
    return ::operator new(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    noteAllocation(bytes);
    return ::operator new[](bytes);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(gridBackingPlanMeasuresEveryRoundedArrayWithoutAllocating)
{
    Ubiquitty::CellGrid grid;
    AllocationObservation observation;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> plan;
    {
        const ObservingBackingAllocations observing(observation);
        plan = grid.backingStoragePlan(17, 33);
    }
    REQUIRE(plan);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(plan->rowCapacity, 32);
    CHECK_EQ(plan->columnCapacity, 48);
    CHECK_EQ(plan->retainedBytes, expectedBackingBytes(32, 48));
    CHECK_EQ(plan->peakBytes, plan->retainedBytes);
    CHECK_EQ(grid.rows(), 0);
    CHECK_EQ(grid.columns(), 0);

    AllocationObservation allocations;
    bool didReserve = false;
    {
        const ObservingBackingAllocations observing(allocations);
        didReserve = grid.reserveCapacity(17, 33, plan->peakBytes);
    }
    REQUIRE(didReserve);
    CHECK_EQ(allocations.calls, std::size_t { 10 });
    CHECK_EQ(allocations.bytes, plan->retainedBytes);
    const auto cells = std::size_t { 32 } * 48;
    const std::array<std::size_t, 10> expectedSizes {
        cells * sizeof(Ubiquitty::Cell), 32 * sizeof(Ubiquitty::Cell*), 32 * sizeof(Ubiquitty::RowState),
        cells * sizeof(Ubiquitty::Cell), 32 * sizeof(Ubiquitty::Cell*), 32 * sizeof(Ubiquitty::RowState),
        cells * sizeof(Ubiquitty::Cell), 32 * 6, 32 * sizeof(bool), 32 * sizeof(std::uint32_t)
    };
    for (std::size_t index = 0; index < expectedSizes.size(); ++index)
        CHECK_EQ(allocations.sizes[index], expectedSizes[index]);
    CHECK_EQ(grid.rows(), 0);
    CHECK_EQ(grid.columns(), 0);
}

TEST(gridBackingAdmissionRefusesBeforeAnyAllocationOrMutation)
{
    Ubiquitty::CellGrid grid;
    grid.init(7, 9);
    grid.setCell(Ubiquitty::Row(2), Ubiquitty::Column(3), markedCell('p'));
    grid.setAlternateActive(Ubiquitty::ScreenMode::Alternate);
    grid.setCell(Ubiquitty::Row(4), Ubiquitty::Column(5), markedCell('a'));
    const auto before = grid.backingStoragePlan(7, 9);
    const auto growth = grid.backingStoragePlan(17, 25);
    REQUIRE(before);
    REQUIRE(growth);
    CHECK_EQ(growth->retainedBytes, expectedBackingBytes(32, 32));
    CHECK_EQ(growth->peakBytes, before->retainedBytes + growth->retainedBytes);

    AllocationObservation observation;
    bool didReserve = true;
    {
        const ObservingBackingAllocations observing(observation);
        didReserve = grid.reserveCapacity(17, 25, growth->peakBytes - 1);
    }
    CHECK(!didReserve);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(grid.rows(), 7);
    CHECK_EQ(grid.columns(), 9);
    CHECK(grid.isAlternateActive());
    const auto* primary = grid.cellAt(Ubiquitty::ScreenMode::Primary, Ubiquitty::Row(2), Ubiquitty::Column(3));
    const auto* alternate = grid.cellAt(Ubiquitty::ScreenMode::Alternate, Ubiquitty::Row(4), Ubiquitty::Column(5));
    REQUIRE(primary);
    REQUIRE(alternate);
    CHECK_EQ(primary->codePoint, std::uint32_t { 'p' });
    CHECK_EQ(alternate->codePoint, std::uint32_t { 'a' });
    const auto after = grid.backingStoragePlan(7, 9);
    REQUIRE(after);
    CHECK(*before == *after);
}

TEST(gridBackingAdmissionIncludesOldAndNewOwnersAtTheExactBoundary)
{
    Ubiquitty::CellGrid grid;
    grid.init(7, 9);
    grid.setCell(Ubiquitty::Row(2), Ubiquitty::Column(3), markedCell('p'));
    const auto before = grid.backingStoragePlan(7, 9);
    const auto growth = grid.backingStoragePlan(17, 25);
    REQUIRE(before);
    REQUIRE(growth);
    CHECK_EQ(growth->peakBytes, before->retainedBytes + growth->retainedBytes);
    AllocationObservation observation;
    bool didReserve = false;
    {
        const ObservingBackingAllocations observing(observation);
        didReserve = grid.reserveCapacity(17, 25, growth->peakBytes);
    }
    REQUIRE(didReserve);
    CHECK_EQ(observation.calls, std::size_t { 10 });
    CHECK_EQ(observation.bytes, growth->retainedBytes);
    CHECK_EQ(grid.rows(), 7);
    CHECK_EQ(grid.columns(), 9);
    const auto* primary = grid.cellAt(Ubiquitty::Row(2), Ubiquitty::Column(3));
    REQUIRE(primary);
    CHECK_EQ(primary->codePoint, std::uint32_t { 'p' });
    const auto held = grid.backingStoragePlan(17, 25);
    REQUIRE(held);
    CHECK_EQ(held->retainedBytes, growth->retainedBytes);
    CHECK_EQ(held->peakBytes, held->retainedBytes);

    AllocationObservation reused;
    {
        const ObservingBackingAllocations observing(reused);
        CHECK(grid.reserveCapacity(17, 25, held->peakBytes));
    }
    CHECK_EQ(reused.calls, std::size_t { 0 });
}

TEST(gridBackingAllowanceRefusalDoesNotMaterializeADeferredRecycledRow)
{
    Ubiquitty::CellGrid grid;
    grid.init(4, 8);
    grid.setCell(Ubiquitty::Row(0), Ubiquitty::Column(2), markedCell('p'));
    grid.setCell(Ubiquitty::Row(1), Ubiquitty::Column(2), markedCell('n'));
    // The backing allocation remains owned throughout this test. Its first
    // row is recycled to the bottom without physically clearing the marker.
    const auto* recycled = grid.cellAt(Ubiquitty::Row(0), Ubiquitty::Column(2));
    REQUIRE(recycled);
    grid.scrollFullViewportUpOneDefaultBlank();
    CHECK_EQ(recycled->codePoint, std::uint32_t { 'p' });
    const auto* logical = grid.logicalCellAt(Ubiquitty::Row(3), Ubiquitty::Column(2));
    REQUIRE(logical);
    CHECK(logical != recycled);
    CHECK_EQ(logical->codePoint, Ubiquitty::defaultBlankCell.codePoint);
    const auto growth = grid.backingStoragePlan(17, 25);
    REQUIRE(growth);

    AllocationObservation observation;
    bool didReserve = true;
    {
        const ObservingBackingAllocations observing(observation);
        didReserve = grid.reserveCapacity(17, 25, growth->peakBytes - 1);
    }
    CHECK(!didReserve);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(grid.rows(), 4);
    CHECK_EQ(grid.columns(), 8);
    CHECK_EQ(recycled->codePoint, std::uint32_t { 'p' });
    CHECK(grid.logicalCellAt(Ubiquitty::Row(3), Ubiquitty::Column(2)) == logical);
    const auto* next = grid.logicalCellAt(Ubiquitty::Row(0), Ubiquitty::Column(2));
    REQUIRE(next);
    CHECK_EQ(next->codePoint, std::uint32_t { 'n' });

    // The materializing accessor proves the retained marker really belongs
    // to that deferred bottom row, rather than unrelated old storage.
    const auto* materialized = grid.cellAt(Ubiquitty::Row(3), Ubiquitty::Column(2));
    REQUIRE(materialized);
    CHECK(materialized == recycled);
    CHECK_EQ(recycled->codePoint, Ubiquitty::defaultBlankCell.codePoint);
}

TEST(gridBackingPlanKeepsTheWholePermittedSparseGeometry)
{
    Ubiquitty::CellGrid grid;
    AllocationObservation observation;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> plan;
    {
        const ObservingBackingAllocations observing(observation);
        plan = grid.backingStoragePlan(Ubiquitty::maxRows, Ubiquitty::maxColumns);
    }
    REQUIRE(plan);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(plan->rowCapacity, Ubiquitty::maxRows);
    CHECK_EQ(plan->columnCapacity, Ubiquitty::maxColumns);
    CHECK_EQ(plan->retainedBytes, expectedBackingBytes(Ubiquitty::maxRows, Ubiquitty::maxColumns));
    CHECK_EQ(plan->peakBytes, plan->retainedBytes);
    CHECK(plan->retainedBytes >= 3 * std::size_t(Ubiquitty::maxRows) * Ubiquitty::maxColumns * sizeof(Ubiquitty::Cell));
}

TEST(gridBackingPlanRefusesArithmeticOverflowBeforeAllocation)
{
    Ubiquitty::CellGrid grid;
    AllocationObservation observation;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> plan;
    {
        const ObservingBackingAllocations observing(observation);
        plan = grid.backingStoragePlan(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    }
    CHECK(!plan);
    if (!plan)
        CHECK(plan.error() == Ubiquitty::CellGrid::BackingStorageError::OverCapacity);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(grid.rows(), 0);
    CHECK_EQ(grid.columns(), 0);
}

TEST(gridBackingPlanRefusesGeometryBeyondTheGridBounds)
{
    // Past either bound is refused before any array is allocated; a negative
    // request plans the one-entry minimum instead of converting to a huge count.
    Ubiquitty::CellGrid grid;
    AllocationObservation observation;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> tall;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> wide;
    std::expected<Ubiquitty::CellGrid::BackingStoragePlan, Ubiquitty::CellGrid::BackingStorageError> negative;
    bool didReserve = true;
    {
        const ObservingBackingAllocations observing(observation);
        tall = grid.backingStoragePlan(Ubiquitty::maxRows + 1, Ubiquitty::maxColumns);
        wide = grid.backingStoragePlan(Ubiquitty::maxRows, Ubiquitty::maxColumns + 1);
        negative = grid.backingStoragePlan(std::numeric_limits<int>::min(), std::numeric_limits<int>::min());
        didReserve = grid.reserveCapacity(Ubiquitty::maxRows + 1, 1);
    }
    CHECK(!tall);
    if (!tall)
        CHECK(tall.error() == Ubiquitty::CellGrid::BackingStorageError::OverCapacity);
    CHECK(!wide);
    if (!wide)
        CHECK(wide.error() == Ubiquitty::CellGrid::BackingStorageError::OverCapacity);
    REQUIRE(negative);
    CHECK_EQ(negative->rowCapacity, 1);
    CHECK_EQ(negative->columnCapacity, 1);
    CHECK_EQ(negative->retainedBytes, expectedBackingBytes(1, 1));
    CHECK(!didReserve);
    CHECK_EQ(observation.calls, std::size_t { 0 });
    CHECK_EQ(grid.rows(), 0);
    CHECK_EQ(grid.columns(), 0);
}

UBIQUITTY_TEST_MAIN()
