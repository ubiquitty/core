// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "CheckpointConnectionFixture.h"

using namespace UbiquittyTest::CheckpointConnection;

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

TEST(defaultDecodedCellAllowanceIsIndependentAndBelowTheLargestLegalGrid)
{
    const auto policy = measuredClientPolicy();
    CHECK(policy.maxDecodedScreenCellBytes == (std::size_t { 32 } << 20));
    CHECK(policy.maxDecodedScreenCellBytes > 0);
    CHECK(policy.maxDecodedScreenCellBytes < 2 * static_cast<std::size_t>(Ubiquitty::maxRows) * static_cast<std::size_t>(Ubiquitty::maxColumns) * sizeof(Ubiquitty::Cell));
    CHECK(policy.maxCheckpointBytes == defaultMaxCheckpointBytes);
    CHECK(policy.maxDecodedScreenCellBytes != policy.maxCheckpointBytes);
}

TEST(firstCheckpointOverCellAllowanceIsRefusedBeforePreparation)
{
    auto source = makeTerminal(7, 23);
    UbiquittyTest::feed(*source, "oversized checkpoint");
    const auto body = checkpointOf(*source);
    const auto cellBytes = checkpointCellStorageBytes(body);
    REQUIRE(cellBytes.has_value());
    REQUIRE(*cellBytes > 0);
    ConnectionFixture fixture(policyWithCellAllowance(*cellBytes - 1));
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    const Ubiquitty::Terminal* destination = fixture.client.paneTerminal(1);
    REQUIRE(destination);
    const auto before = checkpointOf(*destination);
    AllocationObservation observation;
    fixture.checkpoint(1, body, 1, 100, &observation);
    CHECK(observation.attempts.load() == 0);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(fixture.client.counters().checkpointsApplied == 0);
    CHECK(fixture.client.counters().resyncsSent == 1);
    CHECK(checkpointOf(*destination) == before);
    fixture.client.storeReplicas();
    const auto* held = fixture.replicas.find(firstPaneID);
    REQUIRE(held);
    CHECK(!held->hasCheckpoint);
    CHECK(held->replica.generation == 0);
}

TEST(aZeroLiveCellAllowanceDoesNotMeanUnlimited)
{
    auto source = makeTerminal(3, 9);
    const auto body = checkpointOf(*source);
    ConnectionFixture fixture(policyWithCellAllowance(0));
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    AllocationObservation observation;
    fixture.checkpoint(1, body, 1, 100, &observation);
    CHECK(observation.attempts.load() == 0);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(fixture.client.counters().checkpointsApplied == 0);
    CHECK(fixture.client.counters().resyncsSent == 1);
}

TEST(refusedCheckpointPreservesCommittedPictureDictionariesAndHistory)
{
    auto oversized = makeTerminal(7, 23);
    oversized->historyEpoch = 2;
    UbiquittyTest::feed(*oversized, "\x1b[31mreplacement\x1b[0m");
    const auto replacement = checkpointOf(*oversized, 2);
    const auto cellBytes = checkpointCellStorageBytes(replacement);
    REQUIRE(cellBytes.has_value());
    ConnectionFixture fixture(policyWithCellAllowance(*cellBytes - 1));
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    auto original = makeTerminal(3, 9);
    UbiquittyTest::feed(*original, "one\r\ntwo\r\nthree\r\nfour\r\n\x1b[32mold\x1b[0m");
    fixture.checkpoint(1, checkpointOf(*original), 1, 3);
    REQUIRE(fixture.client.counters().checkpointsApplied == 1);
    REQUIRE(fixture.seedHistory(1, *original));
    fixture.client.storeReplicas();
    const auto* held = fixture.replicas.find(firstPaneID);
    REQUIRE(held && held->hasCheckpoint);
    const ScreenReplica beforeReplica = held->replica;
    REQUIRE(beforeReplica.pens().size() > 1);
    const Ubiquitty::Terminal* destination = fixture.client.paneTerminal(1);
    REQUIRE(destination);
    const auto beforePicture = checkpointOf(*destination);
    const HistoryCache* history = fixture.client.historyCache(1);
    REQUIRE(history);
    const auto beforeHistory = history->snapshot(16'384, 16'384);
    REQUIRE(beforeHistory.has_value());
    REQUIRE(history->lineCount() == 1);
    const auto beforeKey = history->key();
    const auto beforeHead = history->head();
    const auto beforePurges = fixture.purges.calls;
    AllocationObservation observation;
    fixture.checkpoint(1, replacement, 2, 100, &observation);
    CHECK(observation.attempts.load() == 0);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(fixture.client.counters().checkpointsApplied == 1);
    CHECK(fixture.client.paneGeneration(1) == 1);
    CHECK(checkpointOf(*destination) == beforePicture);
    CHECK(history->key() == beforeKey);
    CHECK(history->head() == beforeHead);
    CHECK(fixture.purges.calls == beforePurges);
    const auto afterHistory = history->snapshot(16'384, 16'384);
    REQUIRE(afterHistory.has_value());
    CHECK(afterHistory->bytes == beforeHistory->bytes);
    CHECK(afterHistory->retainedBytes == beforeHistory->retainedBytes);
    fixture.client.storeReplicas();
    held = fixture.replicas.find(firstPaneID);
    REQUIRE(held && held->hasCheckpoint);
    checkReplicaUnchanged(held->replica, beforeReplica);
}

TEST(checkpointAtExactDecodedCellAllowanceApplies)
{
    auto source = makeTerminal(7, 23);
    UbiquittyTest::feed(*source, "\x1b[34mexact allowance\x1b[0m");
    const auto body = checkpointOf(*source, 7);
    const auto cellBytes = checkpointCellStorageBytes(body);
    REQUIRE(cellBytes.has_value());
    CHECK(*cellBytes == 2 * 7 * 23 * sizeof(Ubiquitty::Cell));
    ConnectionFixture fixture(policyWithCellAllowance(*cellBytes));
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    AllocationObservation observation;
    fixture.checkpoint(1, body, 1, 100, &observation);
    CHECK(observation.attempts.load() >= 2);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(fixture.client.counters().checkpointsApplied == 1);
    CHECK(fixture.client.counters().resyncsSent == 0);
    CHECK(fixture.client.paneGeneration(1) == 7);
    CHECK(fixture.client.isPaneCurrent(1));
    const Ubiquitty::Terminal* destination = fixture.client.paneTerminal(1);
    REQUIRE(destination);
    CHECK(checkpointOf(*destination, 7) == body);
}

TEST(checkpointCellRefusalRetriesAreBoundedAndDoNotFailOtherPanes)
{
    auto oversized = makeTerminal(7, 23);
    const auto body = checkpointOf(*oversized);
    const auto cellBytes = checkpointCellStorageBytes(body);
    REQUIRE(cellBytes.has_value());
    auto policy = policyWithCellAllowance(*cellBytes - 1);
    policy.maxCheckpointRefusals = 3;
    policy.checkpointRetryMicroseconds = 1'000;
    policy.maxCheckpointRetryMicroseconds = 4'000;
    ConnectionFixture fixture(policy);
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    fixture.bind(2, secondPaneID);
    fixture.checkpoint(1, body, 1, 100);
    CHECK(fixture.client.counters().resyncsSent == 1);
    fixture.checkpoint(1, body, 2, 200);
    CHECK(fixture.client.counters().resyncsSent == 1);
    fixture.client.tick(1'199);
    CHECK(fixture.client.counters().resyncsSent == 1);
    fixture.client.tick(1'200);
    CHECK(fixture.client.counters().resyncsSent == 2);
    fixture.checkpoint(1, body, 3, 1'300);
    const auto failure = fixture.client.paneFailure(1);
    REQUIRE(failure);
    CHECK(failure->find("memory") != std::string_view::npos);
    CHECK(fixture.client.counters().panesFailed == 1);
    fixture.client.tick(1'000'000);
    CHECK(fixture.client.counters().resyncsSent == 2);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    auto sibling = makeTerminal(3, 9);
    UbiquittyTest::feed(*sibling, "alive");
    fixture.checkpoint(2, checkpointOf(*sibling, 4), 4, 1'000'001);
    CHECK(fixture.client.state() == ConnectionState::Attached);
    CHECK(!fixture.client.paneFailure(2).has_value());
    CHECK(fixture.client.paneGeneration(2) == 4);
    CHECK(fixture.client.isPaneCurrent(2));
    CHECK(fixture.client.counters().checkpointsApplied == 1);
}

UBIQUITTY_TEST_MAIN()
