// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "AllocationCount.h"
#include "Harness.h"

// test_posted_dispatcher — the dispatcher a session posts to a frontend's loop
// through (Core/Runtime/PostedDispatcher.h).
//
// Pins: a post hands the loop's immediate entry point a token for the unit,
// and a delayed post hands the delayed entry point the delay and a token;
// tokens reach the loop in the order the posts were made; a loop that keeps
// time runs delayed work in deadline order, each token reaching its own unit
// while slots are reused under it; work whose owner died first runs as
// nothing; and once warm, a post and its run allocate nothing.

#include "Base/WeakPtr.h"
#include "Core/Runtime/PostedDispatcher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>
#include <vector>

using Ubiquitty::Runtime::PostedDispatcher;
using Ubiquitty::Runtime::PostedWorkLoop;
using Ubiquitty::Runtime::PostedWorkOutcome;
using Ubiquitty::Runtime::PostedWorkTable;
using Ubiquitty::Runtime::PostedWorkToken;
using Ubiquitty::Runtime::runPostedSessionWork;

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

/// What a loop that only records was handed. A global because a loop is a
/// pair of plain function pointers with no context.
struct Recorded {
    struct Timer {
        double seconds = 0;
        PostedWorkToken token;
    };
    std::vector<PostedWorkToken> soon;
    std::vector<Timer> later;
};

Recorded& recorded()
{
    static Recorded record;
    return record;
}

void recordSoon(PostedWorkToken token) { recorded().soon.push_back(token); }
void recordAfter(double seconds, PostedWorkToken token) { recorded().later.push_back({ .seconds = seconds, .token = token }); }

PostedWorkLoop recordingLoop() { return { .runSoon = &recordSoon, .runAfter = &recordAfter }; }

/// A loop that keeps time: an immediate post runs on its next turn, and delayed
/// work once the clock reaches its deadline, the earliest first and the
/// earlier posted first among equals.
struct Clocked {
    struct Timer {
        double deadline = 0;
        std::uint64_t sequence = 0;
        PostedWorkToken token;
    };
    double now = 0;
    std::uint64_t nextSequence = 0;
    std::vector<PostedWorkToken> soon;
    std::vector<Timer> timers;

    /// Runs the tokens handed to the immediate entry point, and the ones those runs hand it.
    void turn()
    {
        while (!soon.empty()) {
            const std::vector<PostedWorkToken> due = std::exchange(soon, {});
            for (const PostedWorkToken token : due)
                CHECK(runPostedSessionWork(token) == PostedWorkOutcome::Ran);
        }
    }

    /// Moves the clock on, running each timer at its deadline, and what it
    /// posts on the way.
    void advance(double seconds)
    {
        const double until = now + seconds;
        for (;;) {
            turn();
            const auto next = std::ranges::min_element(timers, [](const Timer& left, const Timer& right) {
                return left.deadline != right.deadline ? left.deadline < right.deadline : left.sequence < right.sequence;
            });
            if (next == timers.end() || next->deadline > until)
                break;
            now = next->deadline;
            const PostedWorkToken token = next->token;
            timers.erase(next);
            CHECK(runPostedSessionWork(token) == PostedWorkOutcome::Ran);
        }
        now = until;
    }
};

Clocked& clocked()
{
    static Clocked loop;
    return loop;
}

void clockedSoon(PostedWorkToken token) { clocked().soon.push_back(token); }
void clockedAfter(double seconds, PostedWorkToken token)
{
    Clocked& loop = clocked();
    loop.timers.push_back({ .deadline = loop.now + seconds, .sequence = loop.nextSequence++, .token = token });
}

PostedWorkLoop clockedLoop() { return { .runSoon = &clockedSoon, .runAfter = &clockedAfter }; }

/// Something a pane owns that posts work holding it weakly, as a session does.
class Owner final : public Ubiquitty::CanMakeWeakPtr {
public:
    int runs = 0;
};

} // namespace

TEST(aPostHandsTheLoopATokenForItsUnit)
{
    recorded() = {};
    PostedDispatcher dispatcher(recordingLoop());
    int runs = 0;
    dispatcher.post([&runs] { ++runs; });
    REQUIRE(recorded().soon.size() == 1);
    CHECK(recorded().later.empty());
    CHECK_EQ(runs, 0);
    CHECK(runPostedSessionWork(recorded().soon.front()) == PostedWorkOutcome::Ran);
    CHECK_EQ(runs, 1);
    recorded() = {};
}

TEST(aDelayedPostHandsTheLoopItsDelayAndAToken)
{
    recorded() = {};
    PostedDispatcher dispatcher(recordingLoop());
    int runs = 0;
    dispatcher.postDelayed(0.25, [&runs] { ++runs; });
    CHECK(recorded().soon.empty());
    REQUIRE(recorded().later.size() == 1);
    CHECK(recorded().later.front().seconds == 0.25);
    CHECK(runPostedSessionWork(recorded().later.front().token) == PostedWorkOutcome::Ran);
    CHECK(runPostedSessionWork(recorded().later.front().token) == PostedWorkOutcome::Refused);
    CHECK_EQ(runs, 1);
    recorded() = {};
}

TEST(tokensReachTheLoopInTheOrderThePostsWereMade)
{
    recorded() = {};
    PostedDispatcher dispatcher(recordingLoop());
    std::vector<int> ran;
    for (int index = 0; index < 32; ++index)
        dispatcher.post([&ran, index] { ran.push_back(index); });
    REQUIRE(recorded().soon.size() == 32);
    for (const PostedWorkToken token : recorded().soon)
        CHECK(runPostedSessionWork(token) == PostedWorkOutcome::Ran);
    std::vector<int> expected(32);
    for (int index = 0; index < 32; ++index)
        expected[static_cast<std::size_t>(index)] = index;
    CHECK(ran == expected);
    recorded() = {};
}

// The table hands back slots as their units run, so the timers posted while
// the clock moves reuse slots whose earlier tokens are spent; each still
// reaches its own unit, at its own deadline.
TEST(theDelayedPathOrdersByDeadline)
{
    clocked() = {};
    PostedDispatcher dispatcher(clockedLoop());
    std::vector<int> ran;
    dispatcher.postDelayed(0.3, [&ran] { ran.push_back(30); });
    dispatcher.postDelayed(0.1, [&ran, &dispatcher] {
        ran.push_back(10);
        // Due at 0.15, between the two timers posted before it.
        dispatcher.postDelayed(0.05, [&ran] { ran.push_back(15); });
        dispatcher.post([&ran] { ran.push_back(11); });
    });
    dispatcher.postDelayed(0.2, [&ran] { ran.push_back(20); });
    dispatcher.postDelayed(0.1, [&ran] { ran.push_back(12); });
    dispatcher.post([&ran] { ran.push_back(0); });

    clocked().advance(0.05);
    CHECK(ran == std::vector<int> { 0 });
    clocked().advance(1.0);
    const std::vector<int> expected { 0, 10, 11, 12, 15, 20, 30 };
    CHECK(ran == expected);
    CHECK(clocked().timers.empty());
    clocked() = {};
}

// The unit holds its owner weakly, so a token the loop still holds when the
// owner goes runs as nothing, and the reference goes with the run.
TEST(workOutlivingItsOwnerRunsAsNothing)
{
    recorded() = {};
    PostedDispatcher dispatcher(recordingLoop());
    const std::size_t pendingBefore = PostedWorkTable::singleton().pendingCount();
    auto owner = std::make_unique<Owner>();
    dispatcher.post([weak = owner->weakPtr()] {
        if (Owner* live = weak.get())
            ++live->runs;
    });
    dispatcher.postDelayed(1.0, [weak = owner->weakPtr()] {
        if (Owner* live = weak.get())
            ++live->runs;
    });
    owner.reset();
    REQUIRE(recorded().soon.size() == 1);
    REQUIRE(recorded().later.size() == 1);
    CHECK(runPostedSessionWork(recorded().soon.front()) == PostedWorkOutcome::Ran);
    CHECK(Ubiquitty::Runtime::discardPostedSessionWork(recorded().later.front().token) == PostedWorkOutcome::Discarded);
    CHECK_EQ(PostedWorkTable::singleton().pendingCount(), pendingBefore);

    // A new owner's work reuses the slots; the spent tokens reach none of it.
    auto next = std::make_unique<Owner>();
    const std::vector<PostedWorkToken> spent { recorded().soon.front(), recorded().later.front().token };
    recorded() = {};
    dispatcher.post([weak = next->weakPtr()] {
        if (Owner* live = weak.get())
            ++live->runs;
    });
    for (const PostedWorkToken token : spent)
        CHECK(runPostedSessionWork(token) == PostedWorkOutcome::Refused);
    CHECK_EQ(next->runs, 0);
    REQUIRE(recorded().soon.size() == 1);
    CHECK(runPostedSessionWork(recorded().soon.front()) == PostedWorkOutcome::Ran);
    CHECK_EQ(next->runs, 1);
    recorded() = {};
}

// Once the table has held as many units as the load does, a post through the
// dispatcher and its run make no allocation: the unit is held inline, its slot
// is reused, and the token is two integers.
TEST(aPostAndItsRunAllocateNothingOnceWarm)
{
    recorded() = {};
    recorded().soon.reserve(64);
    PostedDispatcher dispatcher(recordingLoop());
    auto owner = std::make_unique<Owner>();
    const auto post = [&dispatcher, &owner](std::uint64_t generation) {
        dispatcher.post([weak = owner->weakPtr(), generation] {
            if (Owner* live = weak.get(); live && generation)
                ++live->runs;
        });
    };
    const auto runAll = [] {
        for (const PostedWorkToken token : recorded().soon)
            runPostedSessionWork(token);
        recorded().soon.clear();
    };
    for (std::uint64_t index = 1; index <= 64; ++index)
        post(index);
    runAll();

    std::size_t made = 0;
    {
        const UbiquittyTest::AllocationCount count;
        for (std::uint64_t round = 1; round <= 100; ++round) {
            for (std::uint64_t index = 1; index <= 64; ++index)
                post(index);
            runAll();
        }
        made = count.made();
    }
    CHECK_EQ(made, 0u);
    CHECK_EQ(owner->runs, 64 + 100 * 64);
    recorded() = {};
}

UBIQUITTY_TEST_MAIN()
