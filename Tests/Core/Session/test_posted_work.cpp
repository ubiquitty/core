// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "AllocationCount.h"
#include "Harness.h"

// test_posted_work — the unit a session hands its dispatcher (Core/Session/PostedWork.h).
//
// Pins: a unit runs its work once and is empty after; a move carries the work
// and empties the source; the captures are destroyed exactly once, after the
// run or with an unrun unit; a run survives the container that held the unit
// going away; and holding, moving and running a unit allocate nothing.

#include "Core/Session/PostedWork.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

/// A capture that counts the copies of it alive, so a case can see when the
/// unit holding it lets it go.
class LiveTally {
public:
    explicit LiveTally(int& live)
        : m_live(&live)
    {
        ++*m_live;
    }
    LiveTally(const LiveTally& other)
        : m_live(other.m_live)
    {
        ++*m_live;
    }
    LiveTally(LiveTally&& other) noexcept
        : m_live(other.m_live)
    {
        ++*m_live;
    }
    LiveTally& operator=(const LiveTally&) = delete;
    LiveTally& operator=(LiveTally&&) = delete;
    ~LiveTally() { --*m_live; }

private:
    int* m_live;
};

/// The shapes of the lambdas the cases ask about, spelled as the closure
/// types they compile to.
struct SmallWork {
    void operator()() const { }
};

struct StringWork {
    std::string text;
    void operator()() const { }
};

struct OversizeWork {
    std::array<std::byte, PostedWork::capacity + 1> bytes;
    void operator()() const { }
};

/// What a lambda capturing a `const std::string` by copy is: a const member,
/// which a move can only copy.
struct ConstCaptureWork {
    const std::string text;
    void operator()() const { }
};

static_assert(PostedWork::canHold<SmallWork>);
static_assert(PostedWork::canHold<StringWork>);
static_assert(!PostedWork::canHold<OversizeWork>, "an oversized capture set must be boxed by its producer");
static_assert(!PostedWork::canHold<ConstCaptureWork>, "a const capture copies when the unit is relocated");
static_assert(!std::is_copy_constructible_v<PostedWork>, "a unit has one owner, so it cannot run twice");
static_assert(std::is_nothrow_move_constructible_v<PostedWork>);

} // namespace

TEST(aUnitRunsItsWorkOnceAndIsEmptyAfter)
{
    int runs = 0;
    PostedWork work([&runs] { ++runs; });
    REQUIRE(static_cast<bool>(work));
    std::move(work)();
    CHECK_EQ(runs, 1);
    CHECK(!work);
}

TEST(anEmptyUnitHoldsNoWork)
{
    const PostedWork work;
    CHECK(!work);
}

TEST(aMoveCarriesTheWorkAndEmptiesTheSource)
{
    int runs = 0;
    PostedWork first([&runs] { ++runs; });
    PostedWork second(std::move(first));
    CHECK(!first);
    REQUIRE(static_cast<bool>(second));
    PostedWork third;
    third = std::move(second);
    CHECK(!second);
    std::move(third)();
    CHECK_EQ(runs, 1);
}

TEST(aRunDestroysTheCapturesAsItReturns)
{
    int live = 0;
    int liveDuringRun = -1;
    {
        PostedWork work([tally = LiveTally(live), &live, &liveDuringRun] { liveDuringRun = live; });
        CHECK_EQ(live, 1);
        std::move(work)();
        CHECK_EQ(liveDuringRun, 1);
        CHECK_EQ(live, 0);
    }
    CHECK_EQ(live, 0);
}

TEST(anUnrunUnitDestroysItsCapturesOnce)
{
    int live = 0;
    int runs = 0;
    {
        PostedWork work([tally = LiveTally(live), &runs] { ++runs; });
        PostedWork moved(std::move(work));
        CHECK_EQ(live, 1);
    }
    CHECK_EQ(live, 0);
    CHECK_EQ(runs, 0);

    PostedWork replaced([tally = LiveTally(live), &runs] { ++runs; });
    CHECK_EQ(live, 1);
    replaced = PostedWork([] { });
    CHECK_EQ(live, 0);
    replaced.reset();
    CHECK(!replaced);
    CHECK_EQ(runs, 0);
}

// The run moves the work onto its own frame first, so work that empties the
// queue holding it still reads its captures.
TEST(aRunSurvivesTheQueueThatHeldItGoingAway)
{
    std::vector<PostedWork> queue;
    std::string seen;
    queue.emplace_back([&queue, &seen, text = std::string(64, 'x')] {
        queue.clear();
        queue.shrink_to_fit();
        seen = text;
    });
    std::move(queue.front())();
    CHECK(queue.empty());
    CHECK(seen == std::string(64, 'x'));
}

TEST(holdingMovingAndRunningAUnitAllocateNothing)
{
    std::vector<char> bytes(256, 'b');
    auto lifetime = std::make_shared<int>(7);
    std::size_t seenBytes = 0;
    int seenLifetime = 0;
    std::vector<PostedWork> queue;
    queue.reserve(1);
    std::size_t made = 0;
    {
        const UbiquittyTest::AllocationCount count;
        PostedWork work([weak = std::weak_ptr<int>(lifetime), bytes = std::move(bytes), &seenBytes, &seenLifetime] {
            seenBytes = bytes.size();
            if (const auto held = weak.lock())
                seenLifetime = *held;
        });
        PostedWork moved(std::move(work));
        queue.push_back(std::move(moved));
        std::move(queue.front())();
        made = count.made();
    }
    CHECK_EQ(made, 0u);
    CHECK_EQ(seenBytes, 256u);
    CHECK_EQ(seenLifetime, 7);
}

UBIQUITTY_TEST_MAIN()
