// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Platform/Apple/CFAuto.h"

#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <type_traits>
#include <utility>

namespace {

using Ubiquitty::Apple::UniqueCF;

static_assert(!std::is_copy_constructible_v<UniqueCF<CFArrayRef>>);
static_assert(!std::is_copy_assignable_v<UniqueCF<CFArrayRef>>);
static_assert(std::is_nothrow_move_constructible_v<UniqueCF<CFArrayRef>>);
static_assert(std::is_nothrow_move_assignable_v<UniqueCF<CFArrayRef>>);

struct LifetimeSentinel {
    mutable int retains = 0;
    mutable int releases = 0;
};

const void* retainSentinel(CFAllocatorRef, const void* value)
{
    ++static_cast<const LifetimeSentinel*>(value)->retains;
    return value;
}

void releaseSentinel(CFAllocatorRef, const void* value)
{
    ++static_cast<const LifetimeSentinel*>(value)->releases;
}

UniqueCF<CFArrayRef> makeTrackedArray(const LifetimeSentinel& sentinel)
{
    // The sentinel outlives the array; its release callback observes the real
    // CFArray's destruction without relying on Core Foundation retain counts.
    const CFArrayCallBacks callbacks { 0, retainSentinel, releaseSentinel, nullptr, nullptr };
    std::array<const void*, 1> values { &sentinel };
    return UniqueCF<CFArrayRef>(CFArrayCreate(kCFAllocatorDefault, values.data(), static_cast<CFIndex>(values.size()), &callbacks));
}

TEST(scopeExitReleasesARealCFArrayOnce)
{
    LifetimeSentinel sentinel;
    {
        auto array = makeTrackedArray(sentinel);
        REQUIRE(array);
        CHECK_EQ(CFGetTypeID(array.get()), CFArrayGetTypeID());
        CHECK_EQ(CFArrayGetCount(array.get()), CFIndex { 1 });
        CHECK(CFArrayGetValueAtIndex(array.get(), 0) == &sentinel);
        CHECK_EQ(sentinel.retains, 1);
        CHECK_EQ(sentinel.releases, 0);
    }
    CHECK_EQ(sentinel.releases, 1);
}

TEST(nullOwnersAcceptNoObject)
{
    UniqueCF<CFArrayRef> array;
    CHECK(!array);
    CHECK(array.get() == nullptr);
    array.reset();
    CHECK(array.release() == nullptr);
}

TEST(moveConstructionTransfersTheCFReference)
{
    LifetimeSentinel sentinel;
    {
        auto first = makeTrackedArray(sentinel);
        REQUIRE(first);
        const auto raw = first.get();
        UniqueCF<CFArrayRef> second(std::move(first));
        CHECK(!first);
        CHECK(second.get() == raw);
        CHECK_EQ(sentinel.retains, 1);
        CHECK_EQ(sentinel.releases, 0);
        second.reset();
        CHECK_EQ(sentinel.releases, 1);
    }
    CHECK_EQ(sentinel.releases, 1);
}

TEST(moveAssignmentReleasesOnlyThePreviousCFReference)
{
    LifetimeSentinel previous;
    LifetimeSentinel replacement;
    {
        auto first = makeTrackedArray(previous);
        auto second = makeTrackedArray(replacement);
        REQUIRE(first && second);
        const auto raw = second.get();
        first = std::move(second);
        CHECK(!second);
        CHECK(first.get() == raw);
        CHECK_EQ(previous.releases, 1);
        CHECK_EQ(replacement.retains, 1);
        CHECK_EQ(replacement.releases, 0);
    }
    CHECK_EQ(previous.releases, 1);
    CHECK_EQ(replacement.releases, 1);
}

TEST(resetReleasesEachReplacedCFReferenceOnce)
{
    LifetimeSentinel previous;
    LifetimeSentinel replacement;
    {
        auto array = makeTrackedArray(previous);
        auto next = makeTrackedArray(replacement);
        REQUIRE(array && next);
        const auto raw = next.get();
        array.reset(next.release());
        CHECK(!next);
        CHECK(array.get() == raw);
        CHECK_EQ(previous.releases, 1);
        CHECK_EQ(replacement.releases, 0);
        array.reset();
        CHECK(!array);
        CHECK_EQ(replacement.releases, 1);
        array.reset();
    }
    CHECK_EQ(previous.releases, 1);
    CHECK_EQ(replacement.releases, 1);
}

TEST(releaseHandsTheCFReferenceBackToTheCaller)
{
    LifetimeSentinel sentinel;
    CFArrayRef transferred = nullptr;
    {
        auto array = makeTrackedArray(sentinel);
        REQUIRE(array);
        transferred = array.release();
        CHECK(!array);
        CHECK_EQ(sentinel.releases, 0);
    }
    REQUIRE(transferred);
    CHECK_EQ(sentinel.releases, 0);
    CHECK_EQ(CFArrayGetCount(transferred), CFIndex { 1 });
    CFRelease(transferred);
    CHECK_EQ(sentinel.releases, 1);
}

TEST(anOwnedCFStringKeepsItsRealValueAcrossAMove)
{
    UniqueCF<CFStringRef> first(CFStringCreateWithCString(kCFAllocatorDefault, "owned text", kCFStringEncodingUTF8));
    REQUIRE(first);
    UniqueCF<CFStringRef> second(std::move(first));
    CHECK(!first);
    CHECK_EQ(CFGetTypeID(second.get()), CFStringGetTypeID());
    CHECK_EQ(CFStringGetLength(second.get()), CFIndex { 10 });
    CHECK(CFEqual(second.get(), CFSTR("owned text")));
}

TEST(anOwnedCFErrorRetainsItsDomainAndUserInfo)
{
    LifetimeSentinel sentinel;
    auto array = makeTrackedArray(sentinel);
    REQUIRE(array);
    std::array<const void*, 1> keys { CFSTR("lifetime") };
    std::array<const void*, 1> values { array.get() };
    UniqueCF<CFDictionaryRef> userInfo(CFDictionaryCreate(kCFAllocatorDefault, keys.data(), values.data(), static_cast<CFIndex>(keys.size()), &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    UniqueCF<CFStringRef> domain(CFStringCreateWithCString(kCFAllocatorDefault, "com.example.ownership", kCFStringEncodingUTF8));
    REQUIRE(userInfo && domain);
    UniqueCF<CFErrorRef> error(CFErrorCreate(kCFAllocatorDefault, domain.get(), 42, userInfo.get()));
    REQUIRE(error);
    array.reset();
    userInfo.reset();
    domain.reset();
    CHECK_EQ(sentinel.releases, 0);
    CHECK_EQ(CFErrorGetCode(error.get()), CFIndex { 42 });
    CHECK(CFEqual(CFErrorGetDomain(error.get()), CFSTR("com.example.ownership")));

    UniqueCF<CFDictionaryRef> copiedUserInfo(CFErrorCopyUserInfo(error.get()));
    REQUIRE(copiedUserInfo);
    error.reset();
    CHECK_EQ(sentinel.releases, 0);
    const auto retainedArray = static_cast<CFArrayRef>(CFDictionaryGetValue(copiedUserInfo.get(), CFSTR("lifetime")));
    REQUIRE(retainedArray);
    CHECK_EQ(CFGetTypeID(retainedArray), CFArrayGetTypeID());
    CHECK(CFArrayGetValueAtIndex(retainedArray, 0) == &sentinel);
    copiedUserInfo.reset();
    CHECK_EQ(sentinel.retains, 1);
    CHECK_EQ(sentinel.releases, 1);
}

}

UBIQUITTY_TEST_MAIN()
