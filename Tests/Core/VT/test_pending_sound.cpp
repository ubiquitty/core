// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/VT/Sound.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <array>
#include <new>
#include <vector>

using namespace Ubiquitty;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

PAL::PlayTonesRequest sample(std::span<const int16_t> pcm)
{
    PAL::PlayTonesRequest request;
    request.wave = 6;
    request.sampleSlot = 1;
    request.samplePCM = pcm.data();
    request.sampleFrames = static_cast<uint32_t>(pcm.size());
    return request;
}

TEST(pendingSamplesOwnTheParsedBytesUntilExplicitRemoval)
{
    VT::PendingSound pending;
    std::array<int16_t, 3> bank { -7, 0, 22 };
    REQUIRE(pending.push(sample(bank), 9) == VT::PendingSoundAdmission::Queued);
    const auto* front = pending.front();
    REQUIRE(front);
    CHECK(front->request.samplePCM != bank.data());
    bank.fill(1);
    CHECK_EQ(front->sampleSerial, 9u);
    CHECK_EQ(front->request.sampleFrames, 3u);
    CHECK_EQ(front->request.samplePCM[0], -7);
    CHECK_EQ(front->request.samplePCM[2], 22);
    CHECK_EQ(pending.pcmBytes, 6u);
    pending.pop();
    CHECK(!pending.front());
    CHECK_EQ(pending.pcmBytes, 0u);
}

TEST(exactPendingPCMCapacityRefusesTheNextWholeSoundWithoutChangingTheQueue)
{
    VT::PendingSound pending;
    std::vector<int16_t> bank(VT::sampleMaxFrames, 31);
    std::size_t remaining = VT::PendingSound::maxPCMBytes;
    while (remaining) {
        const std::size_t frames = std::min(remaining / 2, bank.size());
        REQUIRE(pending.push(sample(std::span(bank).first(frames)), 4) == VT::PendingSoundAdmission::Queued);
        remaining -= frames * 2;
    }
    CHECK_EQ(pending.pcmBytes, VT::PendingSound::maxPCMBytes);
    const auto* first = pending.front();
    const int count = pending.count;
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(pending.push(sample(std::span(bank).first(1)), 5) == VT::PendingSoundAdmission::OverLimit);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK(pending.front() == first);
    CHECK_EQ(pending.count, count);
    CHECK_EQ(first->sampleSerial, 4u);
    while (pending.front())
        pending.pop();
    CHECK_EQ(pending.pcmBytes, 0u);
}

TEST(allocationAndDepthRefusalNeverEvictAnAcceptedOperation)
{
    VT::PendingSound pending;
    std::array<int16_t, 1> bank { 42 };
    for (int index = 0; index < VT::PendingSound::depth - 1; ++index)
        REQUIRE(pending.push(sample(bank), static_cast<uint32_t>(index)) == VT::PendingSoundAdmission::Queued);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        CHECK(pending.push(sample(bank), 99) == VT::PendingSoundAdmission::NoMemory);
    }
    CHECK_EQ(pending.count, VT::PendingSound::depth - 1);
    CHECK_EQ(pending.front()->sampleSerial, 0u);
    CHECK_EQ(pending.pcmBytes, 62u);
    REQUIRE(pending.push(sample(bank), 99) == VT::PendingSoundAdmission::Queued);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(pending.push(sample(bank), 100) == VT::PendingSoundAdmission::OverLimit);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK_EQ(pending.front()->sampleSerial, 0u);
    CHECK_EQ(pending.pcmBytes, 64u);
}

TEST(invalidSampleDoesNotAllocateOrAlterAQueuedStop)
{
    VT::PendingSound pending;
    REQUIRE(pending.push({}, 0) == VT::PendingSoundAdmission::Queued);
    auto request = sample({});
    request.sampleFrames = VT::sampleMaxFrames + 1;
    CHECK(pending.push(request, 1) == VT::PendingSoundAdmission::Invalid);
    request.sampleFrames = 1;
    CHECK(pending.push(request, 1) == VT::PendingSoundAdmission::Invalid);
    CHECK_EQ(pending.count, 1);
    CHECK_EQ(pending.pcmBytes, 0u);
    CHECK_EQ(pending.front()->request.volume, 0u);
}

}

UBIQUITTY_TEST_MAIN()
