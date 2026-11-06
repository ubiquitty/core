// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_clusterpool — direct coverage of Ubiquitty::ClusterPool (Core/ClusterPool.h):
// content-deduped interning with dense 1-based ids, O(1) id resolution, the flat
// open-addressing index across its rehash-on-growth threshold, and the wire
// rebuild path (appendDecoded). The pool must GROW to hold every distinct cluster
// without evicting a still-referenced id — the scrollback/reflow corruption this
// class exists to prevent.

#include "Core/ClusterPool.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

using Ubiquitty::ClusterPool;

namespace {

struct AllocationObservation {
    std::size_t refusedOrdinal = 0;
    std::size_t attempts = 0;
    std::size_t bytes = 0;
    std::size_t refusals = 0;
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

    static void* allocate(std::size_t bytes)
    {
        if (auto* observation = active().load()) {
            observation->bytes += bytes;
            if (++observation->attempts == observation->refusedOrdinal) {
                ++observation->refusals;
                return nullptr;
            }
        }
        return ::operator new[](bytes);
    }

private:
    static std::atomic<AllocationObservation*>& active()
    {
        static std::atomic<AllocationObservation*> observation { nullptr };
        return observation;
    }
    AllocationObservation* m_previous;
};

constexpr std::array<std::uint32_t, 3> firstCluster { 0x1F468, 0x200D, 0x1F469 };
constexpr std::array<std::uint32_t, 3> secondCluster { 0x1F469, 0x200D, 0x1F467 };

} // namespace

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::allocate(bytes);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

static std::span<const std::uint32_t> seq(const std::vector<std::uint32_t>& v) { return { v.data(), v.size() }; }

static bool sequenceEquals(ClusterPool& pool, std::uint16_t id, const std::vector<std::uint32_t>& want)
{
    std::span<const std::uint32_t> got = pool.sequence(id);
    if (got.size() != want.size())
        return false;
    for (std::size_t i = 0; i < want.size(); ++i)
        if (got[i] != want[i])
            return false;
    return true;
}

TEST(clusterpoolCases)
{

    // --- empty / degenerate ---
    {
        ClusterPool pool;
        CHECK(pool.empty());
        CHECK(pool.intern({}) == 0); // empty sequence -> id 0 (no entry)
        CHECK(pool.sequence(0).empty()); // id 0 never resolves
        CHECK(pool.sequence(1).empty()); // unknown id -> empty
        CHECK(pool.empty());
    }

    // --- dedup + dense ids + resolution ---
    {
        ClusterPool pool;
        std::vector<std::uint32_t> family = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
        std::vector<std::uint32_t> flag = { 0x1F3F3, 0xFE0F, 0x200D, 0x1F308 };

        const std::uint16_t a = pool.intern(seq(family));
        const std::uint16_t b = pool.intern(seq(flag));
        CHECK(a == 1 && b == 2); // dense 1-based ids
        CHECK(pool.intern(seq(family)) == a); // identical content dedups to the same id
        CHECK(pool.intern(seq(flag)) == b);
        CHECK(pool.size() == 2); // no growth on repeat
        CHECK(sequenceEquals(pool, a, family));
        CHECK(sequenceEquals(pool, b, flag));

        // Order-sensitive: a reversed sequence is a distinct cluster.
        std::vector<std::uint32_t> reversed(family.rbegin(), family.rend());
        const std::uint16_t c = pool.intern(seq(reversed));
        CHECK(c != a && c == 3);
    }

    // --- length clamp at maxCodePoints ---
    {
        ClusterPool pool;
        std::vector<std::uint32_t> tooLong;
        for (int i = 0; i < ClusterPool::maxCodePoints + 5; ++i)
            tooLong.push_back(0x1F600 + i);
        const std::uint16_t id = pool.intern(seq(tooLong));
        CHECK(pool.sequence(id).size() == static_cast<std::size_t>(ClusterPool::maxCodePoints)); // stored truncated
    }

    // --- grow across the rehash threshold: many distinct clusters, all survive ---
    {
        ClusterPool pool;
        const int count = 5000; // well past the initial 16-bucket table => several rehashes
        std::vector<std::uint16_t> ids;
        ids.reserve(count);
        for (int i = 0; i < count; ++i) {
            std::vector<std::uint32_t> s = { 0x1F468, 0x200D, static_cast<std::uint32_t>(0x2000 + i) };
            ids.push_back(pool.intern(seq(s)));
        }
        CHECK(pool.size() == count); // every distinct cluster grew the pool (none evicted)
        // All ids are distinct and every original sequence still resolves — the
        // whole point: a live id is never lost as the pool grows.
        bool allResolve = true, allDistinct = true;
        for (int i = 0; i < count; ++i) {
            std::vector<std::uint32_t> s = { 0x1F468, 0x200D, static_cast<std::uint32_t>(0x2000 + i) };
            if (!sequenceEquals(pool, ids[i], s))
                allResolve = false;
            if (ids[i] != static_cast<std::uint16_t>(i + 1))
                allDistinct = false;
            // Re-interning after all the growth still dedups to the same id.
            if (pool.intern(seq(s)) != ids[i])
                allResolve = false;
        }
        CHECK(allResolve);
        CHECK(allDistinct);
        CHECK(pool.size() == count); // the re-interns above added nothing
    }

    // --- clear resets ids and storage ---
    {
        ClusterPool pool;
        std::vector<std::uint32_t> s = { 0x1F9D1, 0x200D, 0x1F9B2 };
        CHECK(pool.intern(seq(s)) == 1);
        pool.clear();
        CHECK(pool.empty());
        CHECK(pool.intern(seq(s)) == 1); // ids restart at 1 after clear
    }

    // --- appendDecoded (wire rebuild): preserves ids, resolves, and dedups afterward ---
    {
        ClusterPool pool;
        std::vector<std::uint32_t> s7 = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
        std::vector<std::uint32_t> s3 = { 0x1F468, 0x200D, 0x1F469 };
        CHECK(pool.appendDecoded(1, seq(s7)));
        CHECK(pool.appendDecoded(2, seq(s3)));
        CHECK(pool.size() == 2);
        CHECK(sequenceEquals(pool, 1, s7));
        CHECK(sequenceEquals(pool, 2, s3));
        // The index was rebuilt too: interning existing content finds the id.
        CHECK(pool.intern(seq(s7)) == 1);
        CHECK(pool.intern(seq(s3)) == 2);
        // A new cluster mints past the highest decoded id.
        std::vector<std::uint32_t> fresh = { 0x1F3F4, 0x200D, 0x2620, 0xFE0F };
        CHECK(pool.intern(seq(fresh)) == 3);
    }

    // --- appendDecoded holds the wire to the next id ---
    //
    // An entry is found at id - 1, and a lookup that misses that guess walks the
    // whole pool. Ids come off the wire, so a pool of thousands of entries all
    // bearing one id would cost that walk per cell per frame — which is why an id
    // that is not the next one is refused rather than stored under a slot that
    // does not answer to it.
    {
        ClusterPool pool;
        std::vector<std::uint32_t> a = { 0x1F468, 0x200D, 0x1F469 };
        std::vector<std::uint32_t> b = { 0x1F469, 0x200D, 0x1F466 };
        CHECK(pool.appendDecoded(1, seq(a)));
        CHECK_MSG(!pool.appendDecoded(1, seq(b)), "an id the pool already holds is refused");
        CHECK_MSG(!pool.appendDecoded(3, seq(b)), "an id that skips the next one is refused");
        CHECK_MSG(!pool.appendDecoded(0, seq(b)), "id zero names no entry and is refused");
        CHECK_MSG(pool.size() == 1, "a refused entry is not stored");
        CHECK_MSG(pool.appendDecoded(2, seq(b)), "the next id is still taken after a refusal");
        CHECK(sequenceEquals(pool, 2, b));
    }
}

// Distinct two-codepoint clusters, which is the shape a session interns most of:
// a base plus a combining mark. Distinct for every index below 16384 * 256.
static std::vector<std::uint32_t> clusterFor(int index)
{
    return { static_cast<std::uint32_t>(0x4E00 + (index & 0x3FFF)),
        static_cast<std::uint32_t>(0x0300 + (index >> 14)) };
}

// A full pool refuses a new sequence and nothing it holds changes:
// an id names one sequence for the pool's whole life.
TEST(clusterpoolRefusesNewSequencesWhenFullAndKeepsEveryID)
{
    const int ceiling = ClusterPool::maxEntries;

    ClusterPool pool;
    for (int i = 0; i < ceiling; ++i)
        REQUIRE(pool.intern(seq(clusterFor(i))) == i + 1);
    CHECK_MSG(pool.size() == ceiling, "the pool grows to its ceiling and stops");
    CHECK(pool.isFull());
    const std::vector<ClusterPool::Entry> held(pool.entries().begin(), pool.entries().end());

    for (int i = 0; i < ceiling; ++i)
        CHECK_EQ(pool.intern(seq(clusterFor(ceiling + i))), std::uint16_t { 0 });
    CHECK_MSG(pool.size() == ceiling, "refusals mint nothing");

    REQUIRE(pool.entries().size() == held.size());
    CHECK_MSG(std::memcmp(pool.entries().data(), held.data(), held.size() * sizeof(ClusterPool::Entry)) == 0, "no held entry changed");
    int reachable = 0;
    for (const ClusterPool::Entry& entry : held)
        if (pool.intern({ entry.codePoints.data(), entry.length }) == entry.id)
            ++reachable;
    CHECK_MSG(reachable == ceiling, "every held sequence still interns to its own id");
}

TEST(clusterStorageRefusesEachActualOwnerWithoutChangingThePool)
{
    for (std::size_t ordinal = 1; ordinal <= 3; ++ordinal) {
        ClusterPool pool;
        REQUIRE(pool.reserve(1));
        REQUIRE(pool.intern(firstCluster) == 1);
        const auto* entries = pool.entries().data();
        const auto before = pool.entries().front();
        const auto continuation = pool.continuation();
        const auto bytes = pool.allocationBytes();
        AllocationObservation observation { .refusedOrdinal = ordinal };
        {
            ScopedAllocationObservation scope(observation);
            const auto result = pool.reserve(400);
            CHECK(!result && result.error() == ClusterPool::Error::AllocationRefused);
        }
        CHECK_EQ(observation.attempts, ordinal);
        CHECK_EQ(observation.refusals, std::size_t { 1 });
        CHECK(pool.entries().data() == entries);
        CHECK_EQ(pool.capacity(), std::size_t { 1 });
        CHECK_EQ(pool.allocationBytes(), bytes);
        CHECK(std::memcmp(&pool.entries().front(), &before, sizeof(before)) == 0);
        CHECK_EQ(pool.continuation().nextID, continuation.nextID);
        CHECK_EQ(pool.continuation().oldestSlot, continuation.oldestSlot);
        CHECK_EQ(pool.intern(firstCluster), std::uint16_t { 1 });
        CHECK_EQ(pool.intern(secondCluster), std::uint16_t { 2 });
    }
}

TEST(clusterInternRefusalConsumesNeitherAnIDNorAReplacementSlot)
{
    for (std::size_t ordinal = 1; ordinal <= 3; ++ordinal) {
        ClusterPool pool;
        AllocationObservation emptyObservation { .refusedOrdinal = ordinal };
        {
            ScopedAllocationObservation scope(emptyObservation);
            CHECK_EQ(pool.intern(firstCluster), std::uint16_t { 0 });
        }
        CHECK(pool.empty());
        CHECK_EQ(pool.capacity(), std::size_t { 0 });
        CHECK_EQ(pool.continuation().nextID, std::uint16_t { 1 });
        REQUIRE(pool.reserve(1));
        REQUIRE(pool.intern(firstCluster) == 1);
        AllocationObservation growObservation { .refusedOrdinal = ordinal };
        {
            ScopedAllocationObservation scope(growObservation);
            CHECK_EQ(pool.intern(secondCluster), std::uint16_t { 0 });
            CHECK_EQ(pool.intern(firstCluster), std::uint16_t { 1 });
        }
        CHECK_EQ(growObservation.attempts, ordinal);
        CHECK_EQ(pool.size(), 1);
        CHECK_EQ(pool.continuation().nextID, std::uint16_t { 2 });
        CHECK_EQ(pool.continuation().oldestSlot, std::uint16_t { 0 });
        CHECK_EQ(pool.intern(secondCluster), std::uint16_t { 2 });
    }
}

TEST(clusterDecodedRefusalKeepsTheNextIDAndBorrowedGrowthSequence)
{
    for (std::size_t ordinal = 1; ordinal <= 3; ++ordinal) {
        ClusterPool pool;
        REQUIRE(pool.reserve(1));
        REQUIRE(pool.appendDecoded(1, firstCluster));
        AllocationObservation observation { .refusedOrdinal = ordinal };
        {
            ScopedAllocationObservation scope(observation);
            CHECK(!pool.appendDecoded(2, secondCluster));
        }
        CHECK_EQ(observation.refusals, std::size_t { 1 });
        CHECK_EQ(pool.size(), 1);
        CHECK_EQ(pool.continuation().nextID, std::uint16_t { 2 });
        // Decoded entries can repeat content under distinct canonical IDs.
        REQUIRE(pool.appendDecoded(2, pool.sequence(1)));
        CHECK_EQ(pool.sequence(2).size(), firstCluster.size());
        CHECK(std::equal(pool.sequence(2).begin(), pool.sequence(2).end(), firstCluster.begin()));
        CHECK_EQ(pool.continuation().nextID, std::uint16_t { 3 });
    }
}

TEST(clusterStorageBoundsAndExactArrayBytesAreAccounted)
{
    ClusterPool pool;
    AllocationObservation invalid;
    {
        ScopedAllocationObservation scope(invalid);
        CHECK(!pool.reserve(static_cast<std::size_t>(ClusterPool::maxEntries) + 1));
        CHECK(!pool.reserve(std::numeric_limits<std::size_t>::max()));
        CHECK(pool.reserve(0));
        CHECK(!pool.appendDecoded(2, firstCluster));
    }
    CHECK_EQ(invalid.attempts, std::size_t { 0 });
    CHECK_EQ(pool.allocationBytes(), std::size_t { 0 });
    REQUIRE(ClusterPool::storageBytesForCapacity(0));
    CHECK_EQ(*ClusterPool::storageBytesForCapacity(0), std::size_t { 0 });
    CHECK(!ClusterPool::storageBytesForCapacity(std::numeric_limits<std::size_t>::max()));
    for (const auto capacity : { std::size_t { 1 }, std::size_t { 11 }, std::size_t { 12 }, std::size_t { 31 }, std::size_t { 400 } }) {
        AllocationObservation observation;
        {
            ScopedAllocationObservation scope(observation);
            REQUIRE(pool.reserve(capacity));
        }
        CHECK_EQ(observation.attempts, std::size_t { 3 });
        CHECK_EQ(observation.bytes, pool.allocationBytes());
        REQUIRE(ClusterPool::storageBytesForCapacity(capacity));
        CHECK_EQ(observation.bytes, *ClusterPool::storageBytesForCapacity(capacity));
        CHECK_EQ(pool.capacity(), capacity);
        CHECK(pool.empty());
    }
}

TEST(clusterMovedOwnersKeepIndependentEntriesAndContinueAfterClear)
{
    ClusterPool source;
    REQUIRE(source.reserve(7));
    REQUIRE(source.appendDecoded(1, firstCluster));
    REQUIRE(source.appendDecoded(2, secondCluster));
    const auto* entries = source.entries().data();
    const auto bytes = source.allocationBytes();
    ClusterPool destination;
    REQUIRE(destination.intern(secondCluster) == 1);
    AllocationObservation observation { .refusedOrdinal = 1 };
    {
        ScopedAllocationObservation scope(observation);
        destination = std::move(source);
        CHECK(destination.entries().data() == entries);
        CHECK_EQ(destination.intern(firstCluster), std::uint16_t { 1 });
        CHECK_EQ(destination.intern(secondCluster), std::uint16_t { 2 });
        CHECK(source.empty());
        CHECK_EQ(source.capacity(), std::size_t { 0 });
        CHECK_EQ(source.allocationBytes(), std::size_t { 0 });
        CHECK_EQ(source.continuation().nextID, std::uint16_t { 1 });
    }
    CHECK_EQ(observation.attempts, std::size_t { 0 });
    CHECK_EQ(destination.allocationBytes(), bytes);
    CHECK_EQ(destination.continuation().nextID, std::uint16_t { 3 });
    source.clear();
    CHECK(std::equal(destination.sequence(1).begin(), destination.sequence(1).end(), firstCluster.begin()));
    CHECK_EQ(source.intern(firstCluster), std::uint16_t { 1 });
    ClusterPool moved(std::move(destination));
    CHECK(destination.empty());
    CHECK(moved.entries().data() == entries);
    CHECK_EQ(moved.continuation().nextID, std::uint16_t { 3 });
}

TEST(clusterRefusalWhenFullNeedsNoAllocationAndPreservesContinuation)
{
    ClusterPool pool;
    REQUIRE(pool.reserve(ClusterPool::maxEntries));
    for (int index = 0; index < ClusterPool::maxEntries; ++index) {
        const std::array sequence { std::uint32_t { 0x4E00 }, static_cast<std::uint32_t>(index) };
        REQUIRE(pool.intern(sequence) == index + 1);
    }
    const auto bytes = pool.allocationBytes();
    const auto continuation = pool.continuation();
    AllocationObservation observation { .refusedOrdinal = 1 };
    {
        ScopedAllocationObservation scope(observation);
        for (std::uint32_t index = 0; index < 3; ++index) {
            const std::array sequence { std::uint32_t { 0x4E01 }, index };
            CHECK_EQ(pool.intern(sequence), std::uint16_t { 0 });
        }
        const std::array held { std::uint32_t { 0x4E00 }, std::uint32_t { 7 } };
        CHECK_EQ(pool.intern(held), std::uint16_t { 8 });
        CHECK(pool.reserve(ClusterPool::maxEntries));
    }
    CHECK_EQ(observation.attempts, std::size_t { 0 });
    CHECK_EQ(pool.allocationBytes(), bytes);
    CHECK_EQ(pool.continuation().nextID, continuation.nextID);
    CHECK_EQ(pool.continuation().oldestSlot, continuation.oldestSlot);
    ClusterPool moved(std::move(pool));
    const std::array sequence { std::uint32_t { 0x4E02 }, std::uint32_t { 0 } };
    CHECK_EQ(moved.intern(sequence), std::uint16_t { 0 });
    CHECK(moved.isFull());
}

TEST(clusterInternCanGrowFromAnEntryBorrow)
{
    ClusterPool pool;
    REQUIRE(pool.reserve(1));
    REQUIRE(pool.intern(firstCluster) == 1);
    REQUIRE(pool.intern(pool.sequence(1).subspan(1)) == 2);
    CHECK(std::equal(pool.sequence(1).begin(), pool.sequence(1).end(), firstCluster.begin()));
    CHECK(std::equal(pool.sequence(2).begin(), pool.sequence(2).end(), firstCluster.begin() + 1));
    CHECK_EQ(pool.sequence(2).size(), firstCluster.size() - 1);
    CHECK_EQ(pool.continuation().nextID, std::uint16_t { 3 });
}

UBIQUITTY_TEST_MAIN()
