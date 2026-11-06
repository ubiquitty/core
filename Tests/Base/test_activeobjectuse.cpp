// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Base/ActiveObjectUse.h"
#include "Harness.h"

#include <memory>
#include <vector>

using Ubiquitty::ActiveObjectUse;

namespace {

struct Target {
    std::vector<int>& destroyed;
    int value;
    ~Target() { destroyed.push_back(value); }
};

} // namespace

TEST(aReplacementLivesUntilItsOutermostUseReturns)
{
    std::vector<int> destroyed;
    ActiveObjectUse<Target>* active = nullptr;
    auto owner = std::make_unique<Target>(destroyed, 1);
    {
        ActiveObjectUse outer(active, *owner);
        {
            ActiveObjectUse inner(active, *owner);
            ActiveObjectUse<Target>::preserve(active, owner);
            CHECK(!owner);
            CHECK(inner.get() == outer.get());
            CHECK(destroyed.empty());
        }
        CHECK(destroyed.empty());
        CHECK_EQ(outer.get()->value, 1);
    }
    CHECK(active == nullptr);
    CHECK(destroyed == std::vector<int> { 1 });
}

TEST(nestedReplacementsRetainTheExactObjectsBeingUsed)
{
    std::vector<int> destroyed;
    ActiveObjectUse<Target>* active = nullptr;
    auto owner = std::make_unique<Target>(destroyed, 1);
    {
        ActiveObjectUse first(active, *owner);
        ActiveObjectUse<Target>::preserve(active, owner);
        owner = std::make_unique<Target>(destroyed, 2);
        {
            ActiveObjectUse second(active, *owner);
            ActiveObjectUse<Target>::preserve(active, owner);
            owner = std::make_unique<Target>(destroyed, 3);
            CHECK_EQ(first.get()->value, 1);
            CHECK_EQ(second.get()->value, 2);
            CHECK(!ActiveObjectUse<Target>::contains(active, owner.get()));
            CHECK(destroyed.empty());
        }
        CHECK(destroyed == std::vector<int> { 2 });
        CHECK_EQ(first.get()->value, 1);
    }
    CHECK(destroyed == (std::vector<int> { 2, 1 }));
    CHECK_EQ(owner->value, 3);
    owner.reset();
    CHECK(destroyed == (std::vector<int> { 2, 1, 3 }));
}

TEST(anUnusedObjectStaysWithItsOwner)
{
    std::vector<int> destroyed;
    auto owner = std::make_unique<Target>(destroyed, 1);
    ActiveObjectUse<Target>::preserve(nullptr, owner);
    CHECK(owner != nullptr);
    owner.reset();
    CHECK(destroyed == std::vector<int> { 1 });
}

UBIQUITTY_TEST_MAIN()
