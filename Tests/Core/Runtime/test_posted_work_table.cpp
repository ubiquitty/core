// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "AllocationCount.h"
#include "Harness.h"

// test_posted_work_table — Core's table of posted work a frontend loop holds
// tokens for (Core/Runtime/PostedWorkTable.h).
//
// Pins: a token runs its unit once and is refused after; a discarded token
// never runs and its captures go at once; a stale token cannot reach the unit
// a reused slot holds now, nor can one the table never issued; tokens redeem
// in any order, each to its own unit; a unit may post and redeem from inside
// its run, and its captures may post as they die; admissions from many threads
// each get a token of their own; and once the table has held as many units as
// a load needs, admitting and redeeming allocate nothing.

#include "Core/Runtime/PostedWorkTable.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

using Ubiquitty::Runtime::PostedWorkOutcome;
using Ubiquitty::Runtime::PostedWorkTable;
using Ubiquitty::Runtime::PostedWorkToken;
using Ubiquitty::Session::PostedWork;

void* operator new(std::size_t size)
{
    if (void* block = UbiquittyTest::countedAllocation(size))
        return block;
    std::abort();
}
void* operator new[](std::size_t size)
{
    if (void* block = UbiquittyTest::countedAllocation(size))
        return block;
    std::abort();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return UbiquittyTest::countedAllocation(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return UbiquittyTest::countedAllocation(size); }
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { std::free(block); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* block = UbiquittyTest::countedAlignedAllocation(size, alignment))
        return block;
    std::abort();
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    if (void* block = UbiquittyTest::countedAlignedAllocation(size, alignment))
        return block;
    std::abort();
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return UbiquittyTest::countedAlignedAllocation(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return UbiquittyTest::countedAlignedAllocation(size, alignment);
}
void operator delete(void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept { std::free(block); }
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept { std::free(block); }

namespace {

/// A capture that counts the copies of it alive.
class LiveTally {
public:
    explicit LiveTally(int& live)
        : m_live(&live)
    {
        ++*m_live;
    }
    LiveTally(LiveTally&& other) noexcept
        : m_live(other.m_live)
    {
        ++*m_live;
    }
    LiveTally(const LiveTally&) = delete;
    LiveTally& operator=(const LiveTally&) = delete;
    LiveTally& operator=(LiveTally&&) = delete;
    ~LiveTally() { --*m_live; }

private:
    int* m_live;
};

/// A capture that admits a unit to a table as it dies, as a capture holding
/// the last reference to something that posts on its way out does.
class PostsWhenDestroyed {
public:
    PostsWhenDestroyed(PostedWorkTable& table, PostedWorkToken& posted)
        : m_table(&table)
        , m_posted(&posted)
    {
    }
    PostsWhenDestroyed(PostsWhenDestroyed&& other) noexcept
        : m_table(std::exchange(other.m_table, nullptr))
        , m_posted(other.m_posted)
    {
    }
    PostsWhenDestroyed(const PostsWhenDestroyed&) = delete;
    PostsWhenDestroyed& operator=(const PostsWhenDestroyed&) = delete;
    PostsWhenDestroyed& operator=(PostsWhenDestroyed&&) = delete;
    ~PostsWhenDestroyed()
    {
        if (m_table)
            *m_posted = m_table->admit([] { });
    }

private:
    PostedWorkTable* m_table;
    PostedWorkToken* m_posted;
};

} // namespace

TEST(aTokenRunsItsUnitOnceAndIsRefusedAfter)
{
    PostedWorkTable table;
    int runs = 0;
    const PostedWorkToken token = table.admit([&runs] { ++runs; });
    CHECK_EQ(table.pendingCount(), 1u);
    CHECK(table.run(token) == PostedWorkOutcome::Ran);
    CHECK_EQ(runs, 1);
    CHECK_EQ(table.pendingCount(), 0u);
    CHECK(table.run(token) == PostedWorkOutcome::Refused);
    CHECK(table.discard(token) == PostedWorkOutcome::Refused);
    CHECK_EQ(runs, 1);
}

TEST(aDiscardedTokenNeverRunsAndItsCapturesGoAtOnce)
{
    PostedWorkTable table;
    int runs = 0;
    int live = 0;
    const PostedWorkToken token = table.admit([tally = LiveTally(live), &runs] { ++runs; });
    CHECK_EQ(live, 1);
    CHECK(table.discard(token) == PostedWorkOutcome::Discarded);
    CHECK_EQ(live, 0);
    CHECK(table.run(token) == PostedWorkOutcome::Refused);
    CHECK_EQ(runs, 0);
    CHECK_EQ(table.pendingCount(), 0u);
}

TEST(aRunLetsItsCapturesGoAsItReturns)
{
    PostedWorkTable table;
    int live = 0;
    int liveDuringRun = -1;
    const PostedWorkToken token = table.admit([tally = LiveTally(live), &live, &liveDuringRun] { liveDuringRun = live; });
    CHECK(table.run(token) == PostedWorkOutcome::Ran);
    CHECK_EQ(liveDuringRun, 1);
    CHECK_EQ(live, 0);
}

TEST(aStaleTokenCannotReachTheUnitItsSlotHoldsNow)
{
    PostedWorkTable table;
    int first = 0;
    int second = 0;
    const PostedWorkToken stale = table.admit([&first] { ++first; });
    CHECK(table.run(stale) == PostedWorkOutcome::Ran);
    const PostedWorkToken fresh = table.admit([&second] { ++second; });
    // The freed slot is the one reused, so only the generation tells them apart.
    REQUIRE(fresh.slot == stale.slot);
    CHECK(fresh.generation != stale.generation);
    CHECK(table.run(stale) == PostedWorkOutcome::Refused);
    CHECK(table.discard(stale) == PostedWorkOutcome::Refused);
    CHECK_EQ(second, 0);
    CHECK_EQ(table.pendingCount(), 1u);
    CHECK(table.run(fresh) == PostedWorkOutcome::Ran);
    CHECK_EQ(first, 1);
    CHECK_EQ(second, 1);
}

TEST(aTokenTheTableNeverIssuedIsRefused)
{
    PostedWorkTable table;
    CHECK(table.run(PostedWorkToken {}) == PostedWorkOutcome::Refused);
    int runs = 0;
    const PostedWorkToken token = table.admit([&runs] { ++runs; });
    CHECK(table.run({ .slot = token.slot + 1, .generation = token.generation }) == PostedWorkOutcome::Refused);
    CHECK(table.run({ .slot = token.slot, .generation = token.generation + 1 }) == PostedWorkOutcome::Refused);
    CHECK(table.run({ .slot = token.slot, .generation = 0 }) == PostedWorkOutcome::Refused);
    CHECK(table.discard({ .slot = 1'000'000, .generation = 1 }) == PostedWorkOutcome::Refused);
    CHECK_EQ(runs, 0);
    CHECK(table.run(token) == PostedWorkOutcome::Ran);
    CHECK_EQ(runs, 1);
}

TEST(anEmptyUnitIsNotAdmitted)
{
    PostedWorkTable table;
    const PostedWorkToken token = table.admit(PostedWork {});
    CHECK(token == PostedWorkToken {});
    CHECK_EQ(table.pendingCount(), 0u);
    CHECK_EQ(table.slotCount(), 0u);
    CHECK(table.run(token) == PostedWorkOutcome::Refused);
}

TEST(tokensRedeemInAnyOrderEachToItsOwnUnit)
{
    PostedWorkTable table;
    std::vector<int> ran;
    std::vector<PostedWorkToken> tokens;
    for (int index = 0; index < 16; ++index)
        tokens.push_back(table.admit([&ran, index] { ran.push_back(index); }));
    // Every other one first, from the back, then the rest from the front.
    for (int index = 15; index >= 0; index -= 2)
        CHECK(table.run(tokens[static_cast<std::size_t>(index)]) == PostedWorkOutcome::Ran);
    for (int index = 0; index < 16; index += 2)
        CHECK(table.run(tokens[static_cast<std::size_t>(index)]) == PostedWorkOutcome::Ran);
    const std::vector<int> expected { 15, 13, 11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12, 14 };
    CHECK(ran == expected);
    CHECK_EQ(table.pendingCount(), 0u);
}

// A unit runs with no lock held, so it may post, and redeem what it posted,
// as session work does when it hops again.
TEST(aRunMayPostAndRedeemFromInsideIt)
{
    PostedWorkTable table;
    int inner = 0;
    PostedWorkToken posted;
    PostedWorkOutcome innerOutcome = PostedWorkOutcome::Refused;
    const PostedWorkToken outer = table.admit([&table, &inner, &posted, &innerOutcome] {
        posted = table.admit([&inner] { ++inner; });
        innerOutcome = table.run(posted);
    });
    CHECK(table.run(outer) == PostedWorkOutcome::Ran);
    CHECK(innerOutcome == PostedWorkOutcome::Ran);
    CHECK_EQ(inner, 1);
    CHECK_EQ(table.pendingCount(), 0u);
}

// A unit is destroyed with no lock held, so a capture whose destructor posts
// does not deadlock the table, whether the unit ran or was discarded.
TEST(aCaptureMayPostAsItDies)
{
    PostedWorkTable table;
    PostedWorkToken postedByRun;
    const PostedWorkToken ran = table.admit([poster = PostsWhenDestroyed(table, postedByRun)] { });
    CHECK(table.run(ran) == PostedWorkOutcome::Ran);
    CHECK(table.run(postedByRun) == PostedWorkOutcome::Ran);

    PostedWorkToken postedByDiscard;
    const PostedWorkToken discarded = table.admit([poster = PostsWhenDestroyed(table, postedByDiscard)] { });
    CHECK(table.discard(discarded) == PostedWorkOutcome::Discarded);
    CHECK(table.run(postedByDiscard) == PostedWorkOutcome::Ran);
    CHECK_EQ(table.pendingCount(), 0u);
}

TEST(admissionsFromManyThreadsEachGetATokenOfTheirOwn)
{
    constexpr int threadCount = 4;
    constexpr int perThread = 500;
    PostedWorkTable table;
    std::array<int, threadCount * perThread> runs {};
    std::mutex tokensLock;
    std::vector<PostedWorkToken> tokens;
    {
        std::vector<std::jthread> posters;
        for (int thread = 0; thread < threadCount; ++thread) {
            posters.emplace_back([&, thread] {
                for (int index = 0; index < perThread; ++index) {
                    const std::size_t slot = static_cast<std::size_t>(thread * perThread + index);
                    const PostedWorkToken token = table.admit([&runs, slot] { ++runs[slot]; });
                    const std::scoped_lock hold(tokensLock);
                    tokens.push_back(token);
                }
            });
        }
    }
    CHECK_EQ(tokens.size(), static_cast<std::size_t>(threadCount * perThread));
    CHECK_EQ(table.pendingCount(), tokens.size());
    for (const PostedWorkToken token : tokens)
        CHECK(table.run(token) == PostedWorkOutcome::Ran);
    CHECK(std::ranges::all_of(runs, [](int count) { return count == 1; }));
    CHECK_EQ(table.pendingCount(), 0u);
}

// Slots are reused: the table allocates only to hold more units at once than
// it ever has, never per post.
TEST(admittingAndRedeemingAtAKnownDepthAllocateNothing)
{
    constexpr std::size_t depth = 100;
    PostedWorkTable table;
    std::vector<PostedWorkToken> tokens;
    tokens.reserve(depth);
    const auto owner = std::make_shared<int>(0);
    std::size_t delivered = 0;

    // The first burst grows the table to the depth.
    for (std::size_t index = 0; index < depth; ++index)
        tokens.push_back(table.admit([&delivered] { ++delivered; }));
    for (const PostedWorkToken token : tokens)
        CHECK(table.run(token) == PostedWorkOutcome::Ran);
    tokens.clear();
    REQUIRE(table.slotCount() == depth);

    std::size_t made = 0;
    {
        const UbiquittyTest::AllocationCount count;
        for (int round = 0; round < 50; ++round) {
            // A weak reference and a serial: what session work captures.
            for (std::size_t index = 0; index < depth; ++index)
                tokens.push_back(table.admit([&delivered, weak = std::weak_ptr<int>(owner), index] {
                    if (weak.lock() && index < depth)
                        ++delivered;
                }));
            for (const PostedWorkToken token : tokens)
                table.run(token);
            tokens.clear();
            const PostedWorkToken single = table.admit([&delivered] { ++delivered; });
            table.run(single);
        }
        made = count.made();
    }
    CHECK_EQ(made, 0u);
    CHECK_EQ(table.slotCount(), depth);
    CHECK_EQ(delivered, depth + 50 * (depth + 1));
}

// The process's table, which the free functions redeem from, is the one a
// dispatcher admits to.
TEST(theFreeFunctionsRedeemFromTheProcessTable)
{
    PostedWorkTable& table = PostedWorkTable::singleton();
    const std::size_t pendingBefore = table.pendingCount();
    int runs = 0;
    const PostedWorkToken ran = table.admit([&runs] { ++runs; });
    const PostedWorkToken discarded = table.admit([&runs] { ++runs; });
    CHECK_EQ(table.pendingCount(), pendingBefore + 2);
    CHECK(Ubiquitty::Runtime::runPostedSessionWork(ran) == PostedWorkOutcome::Ran);
    CHECK(Ubiquitty::Runtime::discardPostedSessionWork(discarded) == PostedWorkOutcome::Discarded);
    CHECK(Ubiquitty::Runtime::runPostedSessionWork(ran) == PostedWorkOutcome::Refused);
    CHECK_EQ(runs, 1);
    CHECK_EQ(table.pendingCount(), pendingBefore);
}

UBIQUITTY_TEST_MAIN()
