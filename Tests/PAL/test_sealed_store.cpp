// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "PAL/SealedStore.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <algorithm>

using namespace Ubiquitty::PAL;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

// This fixture tests dispatch and bounds, not cryptography. The Swift test
// exercises the actual AEAD. Its deterministic bytes are never persisted.
struct DriverCalls {
    std::size_t count = 0;
    bool shouldRefuse = false;
    bool shouldWriteShort = false;
    std::array<std::uint8_t, 65> scope {};

    static DriverCalls& current()
    {
        static DriverCalls calls;
        return calls;
    }

    static std::ptrdiff_t transform(const std::uint8_t* scope, std::size_t scopeSize, const std::uint8_t*, std::size_t, std::uint8_t* output, std::size_t capacity)
    {
        auto& calls = current();
        ++calls.count;
        if (scopeSize != calls.scope.size() || calls.shouldRefuse)
            return -1;
        std::copy_n(scope, scopeSize, calls.scope.begin());
        std::fill_n(output, capacity, 0x71);
        return static_cast<std::ptrdiff_t>(capacity) - (calls.shouldWriteShort ? 1 : 0);
    }

    static bool forget(const std::uint8_t*, std::size_t) { return !current().shouldRefuse; }
};

SealedStore store()
{
    DriverCalls::current() = {};
    return SealedStore({ .seal = DriverCalls::transform, .open = DriverCalls::transform, .forget = DriverCalls::forget });
}

} // namespace

TEST(aMissingSealerNeverProducesPlaintextFallback)
{
    const SealedStore unavailable;
    const std::array<std::uint8_t, 2> input { 1, 2 };
    const auto result = unavailable.seal({}, input, input.size());
    REQUIRE(!result);
    CHECK(result.error() == SealError::Unavailable);
    CHECK(!unavailable.open({}, input, input.size()));
    CHECK(!unavailable.forget({}));
}

TEST(sealedStoreBudgetsPrecedeAllocationAndDriverDispatch)
{
    const auto sealer = store();
    const std::array<std::uint8_t, 2> input { 1, 2 };
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    CHECK(!sealer.seal({}, input, 1));
    CHECK(!sealer.open({}, input, input.size()));
    CHECK(DriverCalls::current().count == 0);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    const auto nullAllocation = sealer.seal({}, input, input.size());
    REQUIRE(!nullAllocation);
    CHECK(nullAllocation.error() == SealError::AllocationFailed);
    CHECK(DriverCalls::current().count == 0);
}

TEST(sealedStoreReturnsOnlyAnExactSuccessfulOutput)
{
    const auto sealer = store();
    SealScope scope;
    scope.hostInstance[0] = 1;
    scope.principal[0] = 2;
    scope.recordID[0] = 3;
    scope.purpose = SealScope::Purpose::History;
    const std::array<std::uint8_t, 2> input { 1, 2 };
    const auto sealed = sealer.seal(scope, input, input.size());
    REQUIRE(sealed);
    CHECK(sealed->size == input.size() + SealedStore::overhead);
    CHECK(DriverCalls::current().scope == scope.bytes());
    const auto opened = sealer.open(scope, sealed->view(), input.size());
    REQUIRE(opened);
    CHECK(opened->size == input.size());
    DriverCalls::current().shouldWriteShort = true;
    CHECK(!sealer.seal(scope, input, input.size()));
    CHECK(!sealer.open(scope, sealed->view(), input.size()));
    DriverCalls::current().shouldWriteShort = false;
    DriverCalls::current().shouldRefuse = true;
    CHECK(!sealer.open(scope, sealed->view(), input.size()));
    CHECK(!sealer.forget(scope));
}

TEST(emptyPlaintextStillRequiresAnAuthenticatedEnvelope)
{
    const auto sealer = store();
    const auto sealed = sealer.seal({}, {}, 0);
    REQUIRE(sealed);
    CHECK(sealed->size == SealedStore::overhead);
    const auto opened = sealer.open({}, sealed->view(), 0);
    REQUIRE(opened);
    CHECK(opened->view().empty());
}

UBIQUITTY_TEST_MAIN()
