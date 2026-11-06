// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/Input.h"

#include <limits>

using namespace Ubiquitty::Protocol::Sync;

namespace {
constexpr ID128 first { 1, 10 };
constexpr ID128 second { 2, 10 };
constexpr ID128 third { 3, 10 };
constexpr ID128 fourth { 4, 10 };
constexpr PaneGeometry small { 24, 80 };
constexpr PaneGeometry large { 40, 120 };
constexpr PaneGeometry medium { 30, 100 };
constexpr GeometryPolicy policy { 50 };

void compareLedger(const InputLedger& firstLedger, const InputLedger& secondLedger)
{
    for (const auto identity : { first, second, third, fourth }) {
        CHECK(firstLedger.lastWritten(identity) == secondLedger.lastWritten(identity));
        CHECK(firstLedger.lastWrittenAt(identity) == secondLedger.lastWrittenAt(identity));
        CHECK_EQ(firstLedger.settledThrough(identity), secondLedger.settledThrough(identity));
        const auto before = firstLedger.discards(identity);
        const auto after = secondLedger.discards(identity);
        REQUIRE(before.size() == after.size());
        for (std::size_t index = 0; index < before.size(); ++index) {
            CHECK_EQ(before[index].inputWritten, after[index].inputWritten);
            CHECK_EQ(before[index].throughSeq, after[index].throughSeq);
            CHECK_EQ(before[index].serial, after[index].serial);
        }
    }
    CHECK_EQ(firstLedger.discardSerial(), secondLedger.discardSerial());
    CHECK(firstLedger.nextSettleMicroseconds(20) == secondLedger.nextSettleMicroseconds(20));
}
}

TEST(inputContinuationKeepsReplaySettlementAndNextEviction)
{
    InputLedger source(3);
    source.noteWritten(first, 1, 100);
    source.noteWritten(second, 1, 120);
    source.noteDiscarded(first, 3, 140);
    const auto state = source.continuation();
    REQUIRE(state);
    InputLedger restored(3);
    REQUIRE(restored.restoreContinuation(*state));
    compareLedger(source, restored);
    CHECK(restored.admit(first, 3) == InputAdmission::Duplicate);
    CHECK(restored.admit(first, 4) == InputAdmission::Write);
    source.noteReadWouldBlock(139, 20);
    restored.noteReadWouldBlock(139, 20);
    CHECK_EQ(restored.settledThrough(first), 1u);
    CHECK_EQ(restored.settledThrough(second), 0u);
    compareLedger(source, restored);
    source.noteWritten(third, 8, 150);
    restored.noteWritten(third, 8, 150);
    source.noteWritten(fourth, 9, 160);
    restored.noteWritten(fourth, 9, 160);
    CHECK(!restored.lastWritten(second));
    CHECK(restored.lastWritten(first) == 3);
    source.noteReadWouldBlock(180, 20);
    restored.noteReadWouldBlock(180, 20);
    compareLedger(source, restored);
    CHECK(source.takeRecentWriters() == restored.takeRecentWriters());
}

TEST(inputContinuationKeepsCoalescedWritesAndDiscardSerialOrder)
{
    InputLedger source;
    for (std::uint64_t sequence = 1; sequence <= 70; ++sequence)
        source.noteWritten(first, sequence, sequence * 10);
    for (std::uint64_t sequence = 72; sequence <= 100; sequence += 2)
        source.noteDiscarded(first, sequence, sequence * 10);
    source.noteDiscarded(second, 7, 1010);
    const auto state = source.continuation();
    REQUIRE(state);
    CHECK_EQ(state->entries[0].unsettledCount, maxUnsettledWrites);
    CHECK_EQ(state->entries[0].discardCount, maxInputDiscardRecords);
    InputLedger restored;
    REQUIRE(restored.restoreContinuation(*state));
    compareLedger(source, restored);
    source.noteDiscarded(first, 103, 1020);
    restored.noteDiscarded(first, 103, 1020);
    source.noteReadWouldBlock(600, 20);
    restored.noteReadWouldBlock(600, 20);
    compareLedger(source, restored);
    source.noteReadWouldBlock(1040, 20);
    restored.noteReadWouldBlock(1040, 20);
    CHECK_EQ(restored.settledThrough(first), 103u);
    compareLedger(source, restored);
}

TEST(inputRecentWritersCanLegitimatelyOutliveTheirLedgerEntries)
{
    InputLedger source(2);
    source.noteWritten(first, 1, 10);
    source.noteWritten(second, 2, 20);
    source.noteWritten(third, 3, 30);
    REQUIRE(!source.lastWritten(first));
    const auto state = source.continuation();
    REQUIRE(state);
    CHECK(state->recentWriters[0] == first);
    InputLedger restored(2);
    REQUIRE(restored.restoreContinuation(*state));
    CHECK(source.takeRecentWriters() == restored.takeRecentWriters());
    CHECK(restored.takeRecentWriters().empty());
    compareLedger(source, restored);
}

TEST(inputContinuationKeepsTheGlobalWriteClockBeyondTheEntryCount)
{
    InputLedger source(2);
    source.noteWritten(first, 1, 10);
    source.noteWritten(second, 1, 20);
    for (std::uint64_t sequence = 2; sequence <= 10; ++sequence)
        source.noteWritten(first, sequence, sequence * 10 + 20);
    const auto state = source.continuation();
    REQUIRE(state);
    InputLedger restored(2);
    REQUIRE(restored.restoreContinuation(*state));
    source.noteWritten(third, 1, 200);
    restored.noteWritten(third, 1, 200);
    source.noteWritten(fourth, 1, 210);
    restored.noteWritten(fourth, 1, 210);
    REQUIRE(!source.lastWritten(first) && source.lastWritten(third) == 1);
    compareLedger(source, restored);
}

TEST(absoluteSettlementTimesCannotWrapIntoAnEarlierRead)
{
    InputLedger source;
    source.noteWritten(first, 1, UINT64_MAX - 5);
    const auto state = source.continuation();
    REQUIRE(state);
    InputLedger restored;
    REQUIRE(restored.restoreContinuation(*state));
    CHECK(restored.nextSettleMicroseconds(20) == UINT64_MAX);
    source.noteReadWouldBlock(100, 20);
    restored.noteReadWouldBlock(100, 20);
    CHECK_EQ(source.settledThrough(first), 0u);
    CHECK_EQ(restored.settledThrough(first), 0u);
    source.noteReadWouldBlock(UINT64_MAX, 20);
    restored.noteReadWouldBlock(UINT64_MAX, 20);
    CHECK_EQ(source.settledThrough(first), 0u);
    CHECK_EQ(restored.settledThrough(first), 0u);
}

TEST(inputContinuationRefusesMalformedPrefixesBeforeChangingTheReceiver)
{
    InputLedger source(3);
    source.noteWritten(first, 1, 10);
    source.noteWritten(first, 2, 20);
    source.noteDiscarded(first, 4, 30);
    source.noteWritten(second, 1, 40);
    const auto state = source.continuation();
    REQUIRE(state);
    const auto refuses = [&](const InputLedger::Continuation& invalid) {
        InputLedger receiver(3);
        receiver.noteWritten(fourth, 99, 1000);
        CHECK(!receiver.restoreContinuation(invalid));
        CHECK(receiver.lastWritten(fourth) == 99);
        CHECK(!receiver.lastWritten(first));
        CHECK(receiver.takeRecentWriters() == std::vector<ID128> { fourth });
    };
    auto invalid = *state;
    invalid.entryCount = 65;
    refuses(invalid);
    invalid = *state;
    invalid.capacity = 2;
    refuses(invalid);
    invalid = *state;
    invalid.entries[1].clientInstance = first;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].unsettledCount = 65;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].unsettled[1].inputSeq = 1;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].settledSeq = 5;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].order = state->writes + 1;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].discards[0].throughSeq = 5;
    refuses(invalid);
    invalid = *state;
    invalid.entries[0].discards[0].serial = 0;
    refuses(invalid);
    invalid = *state;
    invalid.recentWriters[1] = first;
    refuses(invalid);
    invalid = *state;
    invalid.writes = UINT64_MAX;
    refuses(invalid);
    invalid = *state;
    invalid.discardSerial = UINT64_MAX;
    refuses(invalid);
}

TEST(inputContinuationRetainsEmptyAndSettledStatesWithoutInventingReceipts)
{
    InputLedger source(1);
    const auto empty = source.continuation();
    REQUIRE(empty);
    InputLedger restored(1);
    REQUIRE(restored.restoreContinuation(*empty));
    CHECK(!restored.lastWritten(first));
    source.noteWritten(first, 3, 30);
    source.noteReadWouldBlock(50, 20);
    const auto settled = source.continuation();
    REQUIRE(settled);
    REQUIRE(restored.restoreContinuation(*settled));
    CHECK_EQ(restored.settledThrough(first), 3u);
    CHECK(!restored.nextSettleMicroseconds(20));
    CHECK(restored.lastWrittenAt(first) == 30);
    InputLedger oversized(maxLastWrittenEntries + 1);
    CHECK(!oversized.continuation());
}

TEST(geometryContinuationKeepsThePendingOverrideAndCoalesceDeadline)
{
    GeometryArbiter source(policy);
    source.seed(small);
    source.noteViewport(first, GeometryStanding::Driver, large, 100);
    REQUIRE(source.due(100) == large);
    source.noteViewport(second, GeometryStanding::Driver, small, 120);
    source.noteTyping(second, GeometryStanding::Driver, 120);
    source.overrideSize(third, medium, 130);
    REQUIRE(!source.due(140));
    const auto state = source.continuation();
    REQUIRE(state);
    GeometryArbiter restored(policy);
    REQUIRE(restored.restoreContinuation(*state));
    CHECK(restored.status() == source.status());
    CHECK(restored.applied() == large);
    CHECK(restored.nextDueMicroseconds() == 150);
    CHECK(!restored.due(149));
    CHECK(source.due(150) == restored.due(150));
    source.noteTyping(second, GeometryStanding::Driver, 155);
    restored.noteTyping(second, GeometryStanding::Driver, 155);
    CHECK(source.dueBeforeInput(second, 155) == restored.dueBeforeInput(second, 155));
    CHECK(restored.nextDueMicroseconds() == 200);
    CHECK(source.due(200) == restored.due(200));
    CHECK(restored.applied() == small);
}

TEST(geometryContinuationPreservesWindowPlacementAndLatentPinAttribution)
{
    GeometryArbiter source(policy);
    source.noteViewport(first, GeometryStanding::Driver, small, 100);
    source.noteWindowViewport(first, GeometryStanding::Driver, large, 120);
    source.setPlacement(PanePlacement::BesideOthers);
    source.pin(medium, second);
    source.unpin();
    source.forget(second);
    const auto state = source.continuation();
    REQUIRE(state);
    REQUIRE(state->pinnedBy == second && !state->pin);
    GeometryArbiter restored(policy);
    REQUIRE(restored.restoreContinuation(*state));
    CHECK(source.due(200) == restored.due(200));
    CHECK(restored.applied() == small);
    source.setPlacement(PanePlacement::WholeWindow);
    restored.setPlacement(PanePlacement::WholeWindow);
    CHECK(source.due(250) == restored.due(250));
    CHECK(restored.applied() == large);
    source.pin(medium, second);
    const auto pinned = source.continuation();
    REQUIRE(pinned && restored.restoreContinuation(*pinned));
    CHECK(restored.status().pinnedBy == second);
    CHECK(source.due(300) == restored.due(300));
}

TEST(geometryContinuationRetainsPhysicalTieOrderAndNextEviction)
{
    GeometryArbiter source(policy);
    for (std::uint64_t index = 1; index <= GeometryArbiter::maxHands; ++index)
        source.noteViewport({ index, 10 }, GeometryStanding::Driver, small, 10);
    REQUIRE(source.drivingOwner() == first);
    const auto state = source.continuation();
    REQUIRE(state);
    GeometryArbiter restored(policy);
    REQUIRE(restored.restoreContinuation(*state));
    source.noteViewport({ 65, 10 }, GeometryStanding::Driver, large, 20);
    restored.noteViewport({ 65, 10 }, GeometryStanding::Driver, large, 20);
    source.forget({ 65, 10 });
    restored.forget({ 65, 10 });
    CHECK(restored.drivingOwner() == first);
    source.forget(first);
    restored.forget(first);
    CHECK(restored.drivingOwner() == third);
    const auto before = source.continuation();
    const auto after = restored.continuation();
    REQUIRE(before && after);
    CHECK_EQ(before->handCount, after->handCount);
    for (unsigned index = 0; index < before->handCount; ++index)
        CHECK(before->hands[index].attachmentID == after->hands[index].attachmentID);
}

TEST(geometryContinuationRefusesMalformedStateWithoutApplyingOrChangingOwners)
{
    GeometryArbiter source(policy);
    source.noteViewport(first, GeometryStanding::Driver, small, 100);
    source.noteViewport(second, GeometryStanding::Driver, large, 120);
    REQUIRE(source.due(120));
    const auto state = source.continuation();
    REQUIRE(state);
    const auto refuses = [&](const GeometryArbiter::Continuation& invalid) {
        GeometryArbiter receiver(policy);
        receiver.seed(medium);
        CHECK(!receiver.restoreContinuation(invalid));
        CHECK(receiver.applied() == medium);
        CHECK(!receiver.drivingOwner());
    };
    auto invalid = *state;
    invalid.handCount = 65;
    refuses(invalid);
    invalid = *state;
    invalid.policy.coalesceMicroseconds = 49;
    refuses(invalid);
    invalid = *state;
    invalid.hands[1].attachmentID = first;
    refuses(invalid);
    invalid = *state;
    invalid.hands[0].standing = static_cast<GeometryStanding>(99);
    refuses(invalid);
    invalid = *state;
    invalid.hands[0].viewport = PaneGeometry { 0, 80 };
    refuses(invalid);
    invalid = *state;
    invalid.placement = static_cast<PanePlacement>(99);
    refuses(invalid);
    invalid = *state;
    invalid.appliedAt = UINT64_MAX - 49;
    refuses(invalid);
    invalid = *state;
    invalid.applied.reset();
    refuses(invalid);
}

UBIQUITTY_TEST_MAIN()
