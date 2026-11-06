// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace {

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::ScopedNothrowAllocationRefusal;

constexpr HandoverTextStoreLimits limits { 64 * 1024, 32, 32, 1024 };
constexpr std::array<std::uint32_t, 3> sourceCluster { 0x1F468, 0x200D, 0x1F469 };
constexpr std::array<std::uint32_t, 3> retainedCluster { 0x1F469, 0x200D, 0x1F467 };

} // namespace

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    if (ScopedNothrowAllocationRefusal::refuses(bytes))
        return nullptr;
    return ::operator new[](bytes);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(handoverClusterAllocationRefusalPreservesBothDestinationStores)
{
    Ubiquitty::Terminal source;
    REQUIRE(source.clusterPool.intern(sourceCluster) == 1);
    REQUIRE(source.runTextStore.intern(sourceCluster) == 1);
    auto bytes = encodeHandoverTextStores(source, limits);
    REQUIRE(bytes);
    Ubiquitty::Terminal destination;
    REQUIRE(destination.clusterPool.intern(retainedCluster) == 1);
    REQUIRE(destination.runTextStore.intern(retainedCluster) == 1);
    const auto* clusterOwner = destination.clusterPool.entries().data();
    const auto* runOwner = destination.runTextStore.text(1).data();
    const auto before = encodeHandoverTextStores(destination, limits);
    REQUIRE(before);
    const auto refusals = ScopedNothrowAllocationRefusal::refusalCount();
    {
        ScopedNothrowAllocationRefusal refusal;
        const auto restored = restoreHandoverTextStores(bytes->view(), destination, limits);
        CHECK(!restored && restored.error() == HandoverBodyError::AllocationRefused);
    }
    CHECK_EQ(ScopedNothrowAllocationRefusal::refusalCount(), refusals + 1);
    CHECK(destination.clusterPool.entries().data() == clusterOwner);
    CHECK(destination.runTextStore.text(1).data() == runOwner);
    const auto after = encodeHandoverTextStores(destination, limits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    CHECK_EQ(destination.clusterPool.intern(retainedCluster), std::uint16_t { 1 });
    CHECK_EQ(destination.clusterPool.intern(sourceCluster), std::uint16_t { 2 });
}

TEST(handoverClusterRestorationUsesExactIndependentFinalOwners)
{
    Ubiquitty::Terminal source;
    REQUIRE(source.clusterPool.intern(sourceCluster) == 1);
    REQUIRE(source.clusterPool.intern(retainedCluster) == 2);
    REQUIRE(source.runTextStore.intern(sourceCluster) == 1);
    auto bytes = encodeHandoverTextStores(source, limits);
    REQUIRE(bytes);
    Ubiquitty::Terminal destination;
    const auto attempts = ScopedNothrowAllocationRefusal::attemptCount();
    REQUIRE(restoreHandoverTextStores(bytes->view(), destination, limits));
    CHECK_EQ(ScopedNothrowAllocationRefusal::attemptCount() - attempts, std::size_t { 3 });
    CHECK_EQ(destination.clusterPool.capacity(), std::size_t { 2 });
    REQUIRE(Ubiquitty::ClusterPool::storageBytesForCapacity(2));
    CHECK_EQ(destination.clusterPool.allocationBytes(), *Ubiquitty::ClusterPool::storageBytesForCapacity(2));
    bytes->data.reset();
    source.clusterPool.clear();
    source.runTextStore.clear();
    CHECK(std::ranges::equal(destination.clusterPool.sequence(1), sourceCluster));
    CHECK(std::ranges::equal(destination.clusterPool.sequence(2), retainedCluster));
    CHECK(std::ranges::equal(destination.runTextStore.text(1), sourceCluster));
    CHECK_EQ(destination.clusterPool.continuation().nextID, std::uint16_t { 3 });
    const std::array<std::uint32_t, 2> next { 0x1F600, 0xFE0F };
    CHECK_EQ(destination.clusterPool.intern(next), std::uint16_t { 3 });
    CHECK_EQ(destination.runTextStore.intern(retainedCluster), std::uint16_t { 2 });
}

UBIQUITTY_TEST_MAIN()
