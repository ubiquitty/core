// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_weakptr.cpp — a reference reads as null once its target is gone, and the
// block behind it is released exactly once however the references are copied.
//
// The count is written by hand, as CheckedPtr's is: nothing a compiler generates
// would keep it. Copying, moving and self-assignment are each a way to get it
// wrong, and each is taken below.

#include "Base/WeakPtr.h"

#include <atomic>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Ubiquitty::CanMakeWeakPtr;
using Ubiquitty::WeakPtr;

namespace {

struct Target : CanMakeWeakPtr {
    int value { 0 };
};

/// A target whose CanMakeWeakPtr base does not sit at offset zero.
struct Leading {
    virtual ~Leading() = default;
    int padding { 0 };
};
struct MultiplyBased : Leading, CanMakeWeakPtr {
    int value { 0 };
};

} // namespace

static_assert(sizeof(WeakPtr<Target>) == sizeof(void*),
    "a reference is one pointer, so capturing one costs a pointer");
static_assert(std::is_nothrow_move_constructible_v<WeakPtr<Target>>);
static_assert(std::is_nothrow_default_constructible_v<WeakPtr<Target>>);
static_assert(!std::is_convertible_v<WeakPtr<Target>, bool>,
    "the conversion is explicit, so a reference is not silently a truth value");

TEST(aReferenceToALiveTargetHandsBackTheTarget)
{
    Target target;
    target.value = 7;

    const WeakPtr<Target> reference = target.weakPtr();
    CHECK(reference.get() == &target);
    CHECK(static_cast<bool>(reference));
    CHECK(reference->value == 7);
    CHECK((*reference).value == 7);

    // Two calls name the same target.
    const WeakPtr<Target> second = target.weakPtr();
    CHECK(second == reference);
}

TEST(aReferenceToAGoneTargetReadsAsNull)
{
    WeakPtr<Target> reference;
    CHECK(reference.get() == nullptr);
    CHECK(!static_cast<bool>(reference));

    {
        Target target;
        reference = target.weakPtr();
        CHECK(reference.get() == &target);
    } // the target dies here with a reference outstanding

    // The case the type exists for: a raw pointer would still read as true here.
    CHECK(reference.get() == nullptr);
    CHECK(!static_cast<bool>(reference));
    CHECK(reference == nullptr);
}

TEST(revokingSeversAheadOfDestruction)
{
    Target target;
    const WeakPtr<Target> reference = target.weakPtr();
    CHECK(reference.get() == &target);

    // What a destructor calls before tearing down the fields queued work touches.
    target.revokeWeakPtrs();
    CHECK(reference.get() == nullptr);

    // Revoking twice is not an unbalanced release.
    target.revokeWeakPtrs();
    CHECK(reference.get() == nullptr);

    // Final: a reference taken afterwards reads as null too.
    const WeakPtr<Target> afterwards = target.weakPtr();
    CHECK(afterwards.get() == nullptr);
}

TEST(theBlockSurvivesEveryWayAReferenceIsCopied)
{
    WeakPtr<Target> outlives;
    {
        Target target;
        target.value = 3;

        WeakPtr<Target> first = target.weakPtr();
        WeakPtr<Target> copied(first); // copy construction takes a count
        WeakPtr<Target> moved(std::move(first)); // move takes the count without adding one
        CHECK(moved.get() == &target);
        CHECK(copied.get() == &target);
        CHECK(first.get() == nullptr);

        WeakPtr<Target> assigned;
        assigned = copied;
        CHECK(assigned.get() == &target);

        WeakPtr<Target> movedInto;
        movedInto = std::move(copied);
        CHECK(movedInto.get() == &target);
        CHECK(copied.get() == nullptr);

        // Self-assignment must not release the only reference it holds.
        const WeakPtr<Target>& alias = assigned;
        assigned = alias;
        CHECK(assigned.get() == &target);

        // Assigning null releases this one and leaves the others standing.
        assigned = nullptr;
        CHECK(assigned.get() == nullptr);
        CHECK(movedInto.get() == &target);

        outlives = movedInto;
    } // the target dies with `outlives` still holding a reference

    // Freed exactly when the last reference went. A double release or a leak is
    // what a sanitizer reports here; plainly, the observable is the null.
    CHECK(outlives.get() == nullptr);
}

TEST(aCopiedTargetHandsOutNoReferencesOfItsOwn)
{
    Target original;
    original.value = 11;
    const WeakPtr<Target> toOriginal = original.weakPtr();

    // A defaulted copy would carry the source's block into an object nothing
    // points at, and destroying the copy would sever the source's references.
    Target copied(original);
    CHECK(copied.value == 11);
    CHECK(toOriginal.get() == &original);

    {
        Target moved(std::move(original));
        CHECK(toOriginal.get() == &original); // still the source, not the destination
        const WeakPtr<Target> toMoved = moved.weakPtr();
        CHECK(toMoved.get() == &moved);
        CHECK(toMoved != toOriginal);
    }

    // Destroying both left the source's reference alone.
    CHECK(toOriginal.get() == &original);
}

TEST(aTargetWhoseWeakBaseIsNotFirstHandsBackItself)
{
    MultiplyBased target;
    target.value = 5;

    // The addresses differ, so the case is about the downcast.
    const auto* asWeakBase = static_cast<const CanMakeWeakPtr*>(&target);
    CHECK(static_cast<const void*>(asWeakBase) != static_cast<const void*>(&target));

    const WeakPtr<MultiplyBased> reference = target.weakPtr();
    CHECK(reference.get() == &target);
    CHECK(reference->value == 5);
}

TEST(referencesAreTakenOnOtherThreadsWhileTheOwnerSevers)
{
    // The seam the type is used across: a channel takes references on its I/O
    // thread while the owner's thread severs them. A count that is not atomic
    // loses an increment and frees the block while a reference still reads
    // through it — the thread sanitizer names it at `ref` and `deref`.
    Target target;
    std::atomic<bool> takingHasBegun { false };
    std::atomic<int> taken { 0 };

    std::vector<std::thread> takers;
    takers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        takers.emplace_back([&target, &takingHasBegun, &taken] {
            for (int round = 0; round < 2000; ++round) {
                // Every operation that touches the count, off the owner's thread.
                WeakPtr<Target> reference = target.weakPtr();
                WeakPtr<Target> copied(reference);
                const WeakPtr<Target> moved(std::move(copied));
                reference = moved;
                taken.fetch_add(1, std::memory_order_relaxed);
                takingHasBegun.store(true, std::memory_order_release);
            }
        });
    }

    // Sever while they are still taking: the ordering the destructor creates.
    while (!takingHasBegun.load(std::memory_order_acquire)) {
    }
    target.revokeWeakPtrs();

    for (auto& taker : takers)
        taker.join();

    CHECK(taken.load() == 4 * 2000);

    // Read on the owner's thread, the only one allowed to.
    CHECK(target.weakPtr().get() == nullptr);
}

UBIQUITTY_TEST_MAIN()
