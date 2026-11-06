// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "CheckpointConnectionFixture.h"
#include "CheckpointContentFixture.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/ResumeRecord.h"
#include "Core/Protocol/Sync/SyncClientCore.h"

#include <limits>

using namespace UbiquittyTest::CheckpointConnection;

namespace {

ClientPolicy policyWithStorageAllowance(std::size_t bytes)
{
    auto policy = measuredClientPolicy();
    policy.maxCheckpointStorageCost = bytes;
    return policy;
}

std::size_t admissionCost(std::span<const std::uint8_t> body, const Ubiquitty::Terminal& destination)
{
    const auto plan = checkpointStoragePlan(body);
    CHECK(plan);
    if (!plan)
        return 0;
    const auto cost = checkpointStorageCost(*plan, destination);
    CHECK(cost);
    if (!cost)
        return 0;
    return *cost;
}

struct LentPictures final : ClientPaneDelegate {
    Ubiquitty::Terminal first;
    Ubiquitty::Terminal second;
    unsigned lookups = 0;

    Ubiquitty::Terminal* terminalForPane(ID128 id) override
    {
        ++lookups;
        if (id == firstPaneID)
            return &first;
        return id == secondPaneID ? &second : nullptr;
    }
};

ResumeRecord savedRecord()
{
    return { .hostInstanceID = { 31, 32 }, .clientInstance = { 41, 42 }, .attachmentID = { 51, 52 }, .resumeSecret = { 61, 62 }, .certificatePin = {}, .panes = {}, .drafts = {} };
}

void appendPicture(ResumeRecord& record, const Ubiquitty::Terminal& source, ID128 paneID)
{
    ResumeRecordPane pane { .paneID = paneID, .lastInputSeq = 0, .view = ViewState::Live, .checkpoint = checkpointOf(source), .checkpointDigest = {}, .replica = {} };
    pane.checkpointDigest = digestOf(pane.checkpoint);
    Ubiquitty::Terminal passive;
    REQUIRE(applyCheckpoint(passive, pane.checkpoint, pane.replica) == ApplyOutcome::Applied);
    record.panes.push_back(std::move(pane));
}

} // namespace

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::allocate(bytes, AllocationForm::Object);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    return ScopedAllocationObservation::allocate(bytes, AllocationForm::Array);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(liveCheckpointStorageAdmissionIncludesTheDestinationBacking)
{
    auto source = makeTerminal(7, 23);
    UbiquittyTest::feed(*source, "\x1b[34mnew picture\x1b[0m");
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    const auto body = checkpointOf(*source, 7);
    Ubiquitty::Terminal defaultDestination;
    const auto cost = admissionCost(body, defaultDestination);
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    REQUIRE(cost > plan->preparationStorageCost);
    for (const auto allowance : { std::size_t { 0 }, cost - 1, cost }) {
        ConnectionFixture fixture(policyWithStorageAllowance(allowance));
        REQUIRE(fixture.start());
        fixture.bind(1, firstPaneID);
        const auto* destination = fixture.client.paneTerminal(1);
        REQUIRE(destination);
        const auto before = checkpointOf(*destination);
        AllocationObservation observation;
        fixture.checkpoint(1, body, 1, 100, &observation);
        CHECK(fixture.client.state() == ConnectionState::Attached);
        if (allowance < cost) {
            CHECK(fixture.client.counters().checkpointsApplied == 0);
            CHECK(fixture.client.counters().resyncsSent == 1);
            CHECK(checkpointOf(*destination) == before);
            CHECK(observation.attempts.load() == (allowance ? 1 : 0));
        } else {
            CHECK(fixture.client.counters().checkpointsApplied == 1);
            CHECK(fixture.client.counters().resyncsSent == 0);
            CHECK(checkpointOf(*destination, 7) == body);
        }
    }
}

TEST(destinationGrowthIsRecheckedBeforeAStandaloneCheckpointCommits)
{
    auto source = makeTerminal(33, 97);
    UbiquittyTest::feed(*source, "checkpoint");
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    const auto body = checkpointOf(*source, 9);
    Ubiquitty::Terminal destination;
    const auto cost = admissionCost(body, destination);
    auto prepared = prepareCheckpoint(body, destination, { .maxStorageCost = cost });
    REQUIRE(prepared);
    // Another operation can grow the destination after admission. The final
    // reserve must account for the capacity it actually holds, not an old plan.
    Ubiquitty::resize(destination, 97, 193);
    UbiquittyTest::feed(destination, "must survive");
    const auto before = checkpointOf(destination);
    ScreenReplica replica;
    AllocationObservation observation;
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return commitCheckpoint(destination, std::move(*prepared), replica);
    }();
    CHECK(outcome == ApplyOutcome::AllocationRefused);
    CHECK(observation.attempts.load() == 0);
    CHECK(checkpointOf(destination) == before);
    CHECK(replica.generation == 0);
}

TEST(fullStorageRefusalPreservesThePreviousReplicaAndHistory)
{
    auto replacement = makeTerminal(7, 23);
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*replacement));
    replacement->historyEpoch = 2;
    const auto body = checkpointOf(*replacement, 2);
    Ubiquitty::Terminal defaultDestination;
    const auto cost = admissionCost(body, defaultDestination);
    ConnectionFixture fixture(policyWithStorageAllowance(cost - 1));
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    auto original = makeTerminal(3, 9);
    UbiquittyTest::feed(*original, "one\r\ntwo\r\nthree\r\nfour\r\nold");
    fixture.checkpoint(1, checkpointOf(*original), 1, 3);
    REQUIRE(fixture.client.counters().checkpointsApplied == 1);
    REQUIRE(fixture.seedHistory(1, *original));
    fixture.client.storeReplicas();
    const auto* held = fixture.replicas.find(firstPaneID);
    REQUIRE(held && held->hasCheckpoint);
    const ScreenReplica beforeReplica = held->replica;
    const auto* destination = fixture.client.paneTerminal(1);
    REQUIRE(destination);
    const auto beforePicture = checkpointOf(*destination);
    const auto* history = fixture.client.historyCache(1);
    REQUIRE(history);
    const auto beforeHistory = history->snapshot(16'384, 16'384);
    REQUIRE(beforeHistory);
    const auto beforeKey = history->key();
    const auto beforePurges = fixture.purges.calls;
    AllocationObservation observation;
    fixture.checkpoint(1, body, 2, 100, &observation);
    CHECK(observation.attempts.load() == 1);
    CHECK(fixture.client.counters().checkpointsApplied == 1);
    CHECK(fixture.purges.calls == beforePurges);
    CHECK(history->key() == beforeKey);
    CHECK(checkpointOf(*destination) == beforePicture);
    const auto afterHistory = history->snapshot(16'384, 16'384);
    REQUIRE(afterHistory);
    CHECK(afterHistory->bytes == beforeHistory->bytes);
    fixture.client.storeReplicas();
    held = fixture.replicas.find(firstPaneID);
    REQUIRE(held && held->hasCheckpoint);
    checkReplicaUnchanged(held->replica, beforeReplica);
}

TEST(aGridAllocationRefusalDoesNotPurgeTheLiveHistory)
{
    ConnectionFixture fixture(measuredClientPolicy());
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    auto original = makeTerminal(3, 9);
    UbiquittyTest::feed(*original, "one\r\ntwo\r\nthree\r\nfour\r\nold");
    fixture.checkpoint(1, checkpointOf(*original), 1, 3);
    REQUIRE(fixture.client.counters().checkpointsApplied == 1);
    REQUIRE(fixture.seedHistory(1, *original));
    const auto* destination = fixture.client.paneTerminal(1);
    REQUIRE(destination);
    auto replacement = makeTerminal(33, 97);
    replacement->historyEpoch = 2;
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*replacement));
    const auto body = checkpointOf(*replacement, 2);
    AllocationObservation preparation;
    {
        ScopedAllocationObservation scope(preparation);
        REQUIRE(prepareCheckpoint(body, *destination, {}));
    }
    REQUIRE(preparation.attempts.load() > 0);
    const auto beforePicture = checkpointOf(*destination);
    const auto* history = fixture.client.historyCache(1);
    REQUIRE(history);
    const auto beforeHistory = history->snapshot(16'384, 16'384);
    REQUIRE(beforeHistory);
    const auto beforeKey = history->key();
    const auto beforePurges = fixture.purges.calls;
    AllocationObservation observation { .refusedOrdinal = preparation.attempts.load() + 1 };
    fixture.checkpoint(1, body, 2, 100, &observation);
    CHECK(observation.refusals.load() == 1);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(fixture.client.counters().checkpointsApplied == 1);
    CHECK(fixture.client.paneGeneration(1) == 1);
    CHECK(fixture.purges.calls == beforePurges);
    CHECK(history->key() == beforeKey);
    CHECK(checkpointOf(*destination) == beforePicture);
    const auto afterHistory = history->snapshot(16'384, 16'384);
    REQUIRE(afterHistory);
    CHECK(afterHistory->bytes == beforeHistory->bytes);
}

TEST(coldRestoreStorageAdmissionIsAllOrNothingAcrossPanes)
{
    auto small = makeTerminal(3, 9);
    auto large = makeTerminal(33, 97);
    UbiquittyTest::feed(*small, "small");
    UbiquittyTest::feed(*large, "large");
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*large));
    auto record = savedRecord();
    appendPicture(record, *small, firstPaneID);
    appendPicture(record, *large, secondPaneID);
    LentPictures frontend;
    UbiquittyTest::feed(frontend.first, "first remains");
    UbiquittyTest::feed(frontend.second, "second remains");
    const auto firstBefore = checkpointOf(frontend.first);
    const auto secondBefore = checkpointOf(frontend.second);
    const auto firstCost = admissionCost(record.panes[0].checkpoint, frontend.first);
    const auto secondCost = admissionCost(record.panes[1].checkpoint, frontend.second);
    REQUIRE(firstCost < secondCost);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    constexpr ID128 initialInstance { 71, 72 };
    ClientAttachment attachment(policyWithStorageAllowance(secondCost - 1), measuredLivenessPolicy(), { .clientInstance = initialInstance }, &frontend, &store);
    CHECK(!attachment.restoreFromStore());
    CHECK(!attachment.held());
    CHECK(attachment.clientInstance() == initialInstance);
    CHECK(!attachment.paneTerminal(firstPaneID));
    CHECK(!attachment.paneTerminal(secondPaneID));
    CHECK(checkpointOf(frontend.first) == firstBefore);
    CHECK(checkpointOf(frontend.second) == secondBefore);
    CHECK(store.read().has_value());
}

TEST(coldRestoreAdmitsAnExactStorageAllowance)
{
    auto source = makeTerminal(7, 23);
    UbiquittyTest::feed(*source, "restored");
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    auto record = savedRecord();
    appendPicture(record, *source, firstPaneID);
    LentPictures frontend;
    const auto cost = admissionCost(record.panes[0].checkpoint, frontend.first);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    ClientAttachment attachment(policyWithStorageAllowance(cost), measuredLivenessPolicy(), { .clientInstance = { 71, 72 } }, &frontend, &store);
    REQUIRE(attachment.restoreFromStore());
    CHECK(attachment.held());
    CHECK(checkpointOf(frontend.first) == record.panes[0].checkpoint);
}

TEST(boundedApplyCheckpointPreservesThePictureUntilItsExactAllowanceFits)
{
    auto source = makeTerminal(7, 23);
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    const auto body = checkpointOf(*source, 7);
    Ubiquitty::Terminal destination;
    UbiquittyTest::feed(destination, "retained picture");
    const auto before = checkpointOf(destination);
    const auto cost = admissionCost(body, destination);
    ScreenReplica replica;
    replica.generation = 99;
    for (const auto allowance : { std::size_t { 0 }, cost - 1 }) {
        CHECK(applyCheckpoint(destination, body, replica, { .maxStorageCost = allowance }) == ApplyOutcome::AllocationRefused);
        CHECK(replica.generation == 99);
        CHECK(checkpointOf(destination) == before);
    }
    REQUIRE(applyCheckpoint(destination, body, replica, { .maxStorageCost = cost }) == ApplyOutcome::Applied);
    CHECK(replica.generation == 7);
    CHECK(checkpointOf(destination, 7) == body);
}

TEST(protectedPictureRedrawRefusesFalliblePreparationWithoutReplacingTheDestination)
{
    auto source = makeTerminal(7, 23);
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    auto record = savedRecord();
    appendPicture(record, *source, firstPaneID);
    auto store = std::make_shared<MemoryResumeRecordStore>();
    REQUIRE(store->write(record) == ResumeWriteOutcome::Committed);
    SyncClientCore core(nullptr, nullptr);
    REQUIRE(core.prepareResumeStore(store, record.clientInstance, record.certificatePin));
    Ubiquitty::Terminal destination;
    UbiquittyTest::feed(destination, "retained picture");
    const auto before = checkpointOf(destination);
    AllocationObservation observation { .refusedOrdinal = 1 };
    {
        ScopedAllocationObservation scope(observation);
        CHECK(!core.drawPreparedPane(firstPaneID, destination));
    }
    CHECK(observation.refusals == 1);
    CHECK(checkpointOf(destination) == before);
    REQUIRE(core.drawPreparedPane(firstPaneID, destination));
    CHECK(checkpointOf(destination) == checkpointOf(*source));
}

UBIQUITTY_TEST_MAIN()
