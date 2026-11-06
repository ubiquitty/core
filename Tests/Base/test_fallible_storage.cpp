// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Base/FallibleString.h"
#include "Base/FallibleVector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace {

using Ubiquitty::FallibleString;
using Ubiquitty::FallibleVector;
using Ubiquitty::StorageError;

struct AllocationObservation {
    std::size_t refusedOrdinal = 0;
    std::atomic<std::size_t> attempts { 0 };
    std::atomic<std::size_t> refusals { 0 };
};

class ScopedAllocationObservation {
public:
    explicit ScopedAllocationObservation(AllocationObservation& observation)
        : m_previous(active().exchange(&observation))
    {
    }

    ~ScopedAllocationObservation() { active().store(m_previous); }
    ScopedAllocationObservation(const ScopedAllocationObservation&) = delete;
    ScopedAllocationObservation& operator=(const ScopedAllocationObservation&) = delete;

    static bool shouldRefuse()
    {
        auto* observation = active().load();
        if (!observation || ++observation->attempts != observation->refusedOrdinal)
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

struct OwnerCounts {
    unsigned live = 0;
    unsigned copies = 0;
    unsigned moves = 0;
    unsigned destructions = 0;
    unsigned selfMoves = 0;
};

// The counts outlive their borrowed pointer in every tracked element.
class TrackedOwner {
public:
    TrackedOwner() = default;
    TrackedOwner(OwnerCounts& counts, std::uint32_t value)
        : m_counts(&counts)
        , m_values { value }
    {
        ++m_counts->live;
    }

    TrackedOwner(const TrackedOwner& other)
    {
        if (!tryAssign(other))
            std::abort();
    }

    TrackedOwner& operator=(const TrackedOwner& other)
    {
        if (!tryAssign(other))
            std::abort();
        return *this;
    }

    TrackedOwner(TrackedOwner&& other) noexcept
        : m_counts(std::exchange(other.m_counts, nullptr))
        , m_values(std::move(other.m_values))
    {
        if (m_counts)
            ++m_counts->moves;
    }

    TrackedOwner& operator=(TrackedOwner&& other) noexcept
    {
        if (this == &other) {
            if (m_counts)
                ++m_counts->selfMoves;
            return *this;
        }
        release();
        m_counts = std::exchange(other.m_counts, nullptr);
        m_values = std::move(other.m_values);
        if (m_counts)
            ++m_counts->moves;
        return *this;
    }

    ~TrackedOwner() { release(); }

    [[nodiscard]] std::expected<void, StorageError> tryAssign(const TrackedOwner& other)
    {
        if (this == &other)
            return {};
        if (auto copied = m_values.tryAssign(other.m_values); !copied)
            return copied;
        release();
        m_counts = other.m_counts;
        if (m_counts) {
            ++m_counts->live;
            ++m_counts->copies;
        }
        return {};
    }

    [[nodiscard]] std::uint32_t value() const { return m_values.front(); }
    [[nodiscard]] const std::uint32_t* data() const { return m_values.data(); }

private:
    void release()
    {
        if (m_counts) {
            --m_counts->live;
            ++m_counts->destructions;
            m_counts = nullptr;
        }
    }

    OwnerCounts* m_counts = nullptr;
    FallibleVector<std::uint32_t> m_values;
};

struct alignas(64) AlignedValue {
    std::array<std::uint64_t, 8> words {};
};

} // namespace

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::shouldRefuse() ? nullptr : ::operator new(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::shouldRefuse() ? nullptr : ::operator new[](bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::shouldRefuse() ? nullptr : ::operator new(bytes, alignment);
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::shouldRefuse() ? nullptr : ::operator new[](bytes, alignment);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete[](pointer, alignment); }

TEST(emptyFallibleOwnersNeedNoAllocation)
{
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        FallibleVector<std::uint32_t> vector;
        FallibleString text;
        REQUIRE(vector.tryReserve(0));
        REQUIRE(vector.tryResize(0));
        REQUIRE(vector.tryAssign(vector.begin(), vector.end()));
        REQUIRE(text.tryAssign(std::string_view {}));
        REQUIRE(text.tryResize(0));
        CHECK(vector.empty() && vector.capacity() == 0);
        CHECK(text.empty() && text.capacity() == 0);
        CHECK(text.c_str()[0] == '\0');
        CHECK(text.allocationBytes() == 0);
    }
    CHECK(observation.attempts.load() == 0);
}

TEST(fallibleReservationIsExactAndOverflowIsRefusedBeforeAllocation)
{
    FallibleVector<std::uint32_t> values;
    REQUIRE(values.tryReserve(7));
    CHECK(values.capacity() == 7);
    CHECK(values.allocationBytes() == 7 * sizeof(std::uint32_t));
    CHECK(values.empty());
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        REQUIRE(values.tryResize(3));
        REQUIRE(values.tryReserve(2));
        const auto refused = values.tryReserve(std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) + 1);
        REQUIRE(!refused);
        CHECK(refused.error() == StorageError::OverCapacity);
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(values.capacity() == 7);
    CHECK(values.size() == 3);
    CHECK(values[0] == 0 && values[1] == 0 && values[2] == 0);
}

TEST(fallibleOwnerReservationHonorsOverAlignmentAndCanRefuse)
{
    FallibleVector<AlignedValue> values;
    AllocationObservation refused { .refusedOrdinal = 1 };
    {
        ScopedAllocationObservation scope(refused);
        const auto outcome = values.tryReserve(2);
        REQUIRE(!outcome);
        CHECK(outcome.error() == StorageError::AllocationRefused);
    }
    CHECK(refused.refusals.load() == 1);
    CHECK(values.data() == nullptr && values.capacity() == 0);
    REQUIRE(values.tryResize(2));
    CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(AlignedValue) == 0);
    CHECK(values.allocationBytes() == 2 * sizeof(AlignedValue));
    values[1].words[7] = 42;
    REQUIRE(values.tryReserve(3));
    CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(AlignedValue) == 0);
    CHECK(values[1].words[7] == 42);
}

TEST(fallibleReserveRefusalKeepsValuesCapacityAndPointers)
{
    FallibleVector<std::uint32_t> values { 11, 22, 33 };
    const auto* before = values.data();
    const auto capacity = values.capacity();
    AllocationObservation observation { .refusedOrdinal = 1 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return values.tryReserve(capacity + 1);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == StorageError::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    CHECK(values.data() == before);
    CHECK(values.capacity() == capacity);
    CHECK((values == FallibleVector<std::uint32_t> { 11, 22, 33 }));
}

TEST(fallibleNestedCopiesCleanEveryPartialOwnerAndKeepTheirDestination)
{
    OwnerCounts counts;
    {
        FallibleVector<TrackedOwner> source;
        REQUIRE(source.tryReserve(2));
        REQUIRE(source.tryEmplaceBack(counts, 11));
        REQUIRE(source.tryEmplaceBack(counts, 22));
        FallibleVector<TrackedOwner> destination;
        REQUIRE(destination.tryEmplaceBack(counts, 99));
        const auto* before = destination.data();
        const auto* beforeText = destination.front().data();
        REQUIRE(counts.live == 3);
        for (const std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 }, std::size_t { 3 } }) {
            AllocationObservation observation { .refusedOrdinal = ordinal };
            const auto outcome = [&] {
                ScopedAllocationObservation scope(observation);
                return destination.tryAssign(source);
            }();
            REQUIRE(!outcome);
            CHECK(outcome.error() == StorageError::AllocationRefused);
            CHECK(observation.refusals.load() == 1);
            CHECK(destination.data() == before);
            CHECK(destination.front().data() == beforeText);
            CHECK(destination.size() == 1 && destination.front().value() == 99);
            CHECK(source[0].value() == 11 && source[1].value() == 22);
            CHECK(counts.live == 3);
        }
        REQUIRE(destination.tryAssign(source));
        CHECK(destination.capacity() == source.size());
        CHECK(destination[0].value() == 11 && destination[1].value() == 22);
        CHECK(destination[0].data() != source[0].data());
        CHECK(counts.live == 4);
    }
    CHECK(counts.live == 0);
}

TEST(fallibleOwnerMovesTransferPointersAndDestroyEachElementOnce)
{
    OwnerCounts counts;
    {
        FallibleVector<TrackedOwner> original;
        REQUIRE(original.tryEmplaceBack(counts, 11));
        REQUIRE(original.tryEmplaceBack(counts, 22));
        auto copy = original;
        CHECK(counts.live == 4);
        const auto* before = original.data();
        FallibleVector<TrackedOwner> moved(std::move(original));
        CHECK(moved.data() == before);
        CHECK(original.empty() && original.data() == nullptr);
        CHECK(counts.live == 4);
        copy = std::move(moved);
        CHECK(copy.data() == before);
        CHECK(moved.empty() && moved.data() == nullptr);
        CHECK(counts.live == 2);
        copy.clear();
        CHECK(counts.live == 0);
        CHECK(copy.capacity() >= 2);
    }
    CHECK(counts.live == 0);
    CHECK(counts.destructions == 4);
}

TEST(fallibleVectorSelfSlicesAndEmptyErasePreserveTheirElements)
{
    FallibleVector<std::uint32_t> values { 11, 22, 33, 44 };
    const auto capacity = values.capacity();
    REQUIRE(values.tryAssign(values.begin() + 1, values.end()));
    CHECK((values == FallibleVector<std::uint32_t> { 22, 33, 44 }));
    CHECK(values.capacity() == capacity);
    OwnerCounts counts;
    FallibleVector<TrackedOwner> owned;
    REQUIRE(owned.tryEmplaceBack(counts, 7));
    const auto* pointer = owned.front().data();
    owned.erase(owned.begin(), owned.begin());
    CHECK(counts.selfMoves == 0);
    CHECK(owned.size() == 1 && owned.front().value() == 7);
    CHECK(owned.front().data() == pointer);
}

TEST(fallibleVectorAppendAndResizeMayBorrowOneOfTheirOwnElements)
{
    FallibleVector<std::uint32_t> values { 71 };
    REQUIRE(values.capacity() == 1);
    REQUIRE(values.tryPushBack(values.front()));
    CHECK(values.size() == 2 && values[0] == 71 && values[1] == 71);
    REQUIRE(values.tryResize(4, values.front()));
    CHECK(values[2] == 71 && values[3] == 71);
    FallibleVector<std::uint32_t> emplaced { 93 };
    REQUIRE(emplaced.tryEmplaceBack(emplaced.front()));
    CHECK(emplaced.size() == 2 && emplaced[0] == 93 && emplaced[1] == 93);
}

TEST(fallibleNestedSelfEmplacementRefusalDoesNotConsumeItsSource)
{
    FallibleVector<FallibleString> values;
    REQUIRE(values.tryReserve(1));
    FallibleString prepared;
    REQUIRE(prepared.tryAssign("retained nested owner"));
    REQUIRE(values.tryPushBack(std::move(prepared)));
    REQUIRE(values.size() == values.capacity());
    const auto* vectorOwner = values.data();
    const auto* stringOwner = values.front().data();
    AllocationObservation observation { .refusedOrdinal = 1 };
    const auto refused = [&] {
        ScopedAllocationObservation scope(observation);
        return values.tryEmplaceBack(std::move(values.front()));
    }();
    REQUIRE(!refused);
    CHECK(refused.error() == StorageError::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    CHECK(values.size() == 1 && values.capacity() == 1);
    CHECK(values.data() == vectorOwner);
    CHECK(values.front().data() == stringOwner);
    CHECK(values.front().view() == "retained nested owner");

    REQUIRE(values.tryEmplaceBack(std::move(values.front())));
    CHECK(values.size() == 2);
    CHECK(values.front().empty());
    CHECK(values.back().data() == stringOwner);
    CHECK(values.back().view() == "retained nested owner");
}

TEST(fallibleNestedCopyEmplacementReportsEachAllocationRefusal)
{
    FallibleVector<FallibleString> values;
    REQUIRE(values.tryReserve(1));
    FallibleString prepared;
    REQUIRE(prepared.tryAssign("retained nested owner"));
    REQUIRE(values.tryPushBack(std::move(prepared)));
    const auto* vectorOwner = values.data();
    const auto* stringOwner = values.front().data();
    for (const std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 } }) {
        AllocationObservation observation { .refusedOrdinal = ordinal };
        const auto refused = [&] {
            ScopedAllocationObservation scope(observation);
            return values.tryEmplaceBack(values.front());
        }();
        REQUIRE(!refused);
        CHECK(refused.error() == StorageError::AllocationRefused);
        CHECK(observation.refusals.load() == 1);
        CHECK(values.size() == 1 && values.capacity() == 1);
        CHECK(values.data() == vectorOwner);
        CHECK(values.front().data() == stringOwner);
        CHECK(values.front().view() == "retained nested owner");
    }
    REQUIRE(values.tryEmplaceBack(values.front()));
    CHECK(values.size() == 2);
    CHECK(values.front().data() == stringOwner);
    CHECK(values.front().view() == values.back().view());
    CHECK(values.front().data() != values.back().data());
}

TEST(emptyFallibleInsertIsAnAllocationFreeNoOpForEmptyAndOwnedVectors)
{
    const std::array<std::uint32_t, 0> emptyRange {};
    FallibleVector<std::uint32_t> empty;
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        CHECK(empty.insert(empty.begin(), emptyRange.begin(), emptyRange.end()) == empty.begin());
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(empty.empty() && empty.capacity() == 0 && empty.data() == nullptr);

    OwnerCounts counts;
    FallibleVector<TrackedOwner> owned;
    REQUIRE(owned.tryEmplaceBack(counts, 7));
    const auto* owner = owned.data();
    const auto* nestedOwner = owned.front().data();
    const auto moves = counts.moves;
    CHECK(owned.insert(owned.end(), owned.begin(), owned.begin()) == owned.end());
    CHECK(owned.data() == owner && owned.front().data() == nestedOwner);
    CHECK(counts.moves == moves && counts.selfMoves == 0);
    CHECK(owned.size() == 1 && owned.front().value() == 7);
}

TEST(fallibleStringAssignmentOwnsItsTerminatorAtExactCapacity)
{
    FallibleString text;
    REQUIRE(text.tryAssign("hello"));
    CHECK(text.view() == "hello");
    CHECK(text.capacity() == 5);
    CHECK(text.allocationBytes() == 6);
    CHECK(text.c_str()[text.size()] == '\0');
    const auto* pointer = text.data();
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        text.clear();
        CHECK(text.empty() && text.c_str()[0] == '\0');
        CHECK(text.allocationBytes() == 6);
        REQUIRE(text.tryAssign("again"));
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(text.data() == pointer);
    CHECK(text.view() == "again");
    CHECK(text.c_str()[text.size()] == '\0');
}

TEST(fallibleStringSelfSlicesAndGrowthKeepTheirBytesAndTerminator)
{
    FallibleString text("abcdef");
    const auto* pointer = text.data();
    const auto allocation = text.allocationBytes();
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        REQUIRE(text.tryAssign(text.view().substr(2, 3)));
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(text.data() == pointer && text.allocationBytes() == allocation);
    CHECK(text.view() == "cde" && text.c_str()[3] == '\0');
    text.append(text.view());
    CHECK(text.view() == "cdecde" && text.c_str()[6] == '\0');
    REQUIRE(text.tryResize(8));
    CHECK(text.size() == 8 && text[6] == '\0' && text[7] == '\0');
    CHECK(text.c_str()[8] == '\0');
    text.erase(1, 2);
    CHECK(text.size() == 6 && text.view().substr(0, 4) == "ccde");
    CHECK(text.c_str()[6] == '\0');
}

TEST(fallibleStringRefusalAndOverflowKeepExistingBytesAndCapacity)
{
    FallibleString text("old");
    const auto* pointer = text.data();
    const auto allocation = text.allocationBytes();
    AllocationObservation observation { .refusedOrdinal = 1 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return text.tryAssign("a longer replacement");
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == StorageError::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    CHECK(text.data() == pointer && text.allocationBytes() == allocation);
    CHECK(text.view() == "old" && text.c_str()[3] == '\0');
    AllocationObservation overflow;
    {
        ScopedAllocationObservation scope(overflow);
        const auto refused = text.tryResize(std::numeric_limits<std::size_t>::max());
        REQUIRE(!refused);
        CHECK(refused.error() == StorageError::OverCapacity);
    }
    CHECK(overflow.attempts.load() == 0);
    CHECK(text.view() == "old" && text.c_str()[3] == '\0');
}

UBIQUITTY_TEST_MAIN()
