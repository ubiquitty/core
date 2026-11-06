// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/ResumeRecord.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

using namespace Ubiquitty::Protocol::Sync;

constexpr ID128 firstPaneID { 11, 12 };
constexpr ID128 secondPaneID { 13, 14 };
constexpr ID128 initialClientInstance { 21, 22 };
constexpr std::uint64_t storedGeneration = 37;
constexpr std::size_t decodedCellBytes = 2 * 7 * 23 * sizeof(Ubiquitty::Cell);

/// Paints distinct corners without parser, clock, or platform callbacks.
void paint(Ubiquitty::Terminal& terminal, int rows, int columns, std::uint32_t first)
{
    Ubiquitty::resize(terminal, rows, columns);
    auto cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = first;
    terminal.grid->setCell(Ubiquitty::Row(0), Ubiquitty::Column(0), cell);
    cell.codePoint = first + 1;
    terminal.grid->setCell(Ubiquitty::Row(rows - 1), Ubiquitty::Column(columns - 1), cell);
}

/// Creates fixed process-local resume identities without opening a connection.
ResumeRecord emptyRecord()
{
    return { .hostInstanceID = { 31, 32 }, .clientInstance = { 41, 42 }, .attachmentID = { 51, 52 }, .resumeSecret = { 61, 62 }, .certificatePin = {}, .panes = {}, .drafts = {} };
}

/// Appends one canonical stored picture and its corresponding replica state.
void appendPicture(ResumeRecord& record, const Ubiquitty::Terminal& source, ID128 paneID)
{
    ResumeRecordPane pane { .paneID = paneID, .lastInputSeq = 17, .view = ViewState::Live, .checkpoint = UbiquittyTest::checkpointBody(source, storedGeneration), .checkpointDigest = {}, .replica = {} };
    CHECK(!pane.checkpoint.empty());
    if (pane.checkpoint.empty())
        return;
    pane.checkpointDigest = digestOf(pane.checkpoint);
    Ubiquitty::Terminal passive;
    const auto outcome = applyCheckpoint(passive, pane.checkpoint, pane.replica);
    CHECK(outcome == ApplyOutcome::Applied);
    if (outcome == ApplyOutcome::Applied)
        record.panes.push_back(std::move(pane));
}

/// Gives encoded bodies ample room while configuring only the decoded-cell cap.
ClientPolicy policyFor(std::size_t cellBytes)
{
    auto policy = measuredClientPolicy();
    policy.maxCheckpointBytes = std::size_t { 1 } << 20;
    policy.maxDecodedScreenCellBytes = cellBytes;
    return policy;
}

/// Supplies preexisting frontend pictures and records every destination lookup.
struct LentPictures final : ClientPaneDelegate {
    Ubiquitty::Terminal first;
    Ubiquitty::Terminal second;
    unsigned lookups = 0;

    /// Seeds distinguishable frontend pictures before any restore can run.
    LentPictures()
    {
        paint(first, 3, 11, 'a');
        paint(second, 4, 13, 'm');
    }

    /// Returns the exact pane destination without reentering the attachment.
    Ubiquitty::Terminal* terminalForPane(ID128 paneID) override
    {
        ++lookups;
        if (paneID == firstPaneID)
            return &first;
        return paneID == secondPaneID ? &second : nullptr;
    }
};

/// Checks that refusal published neither a picture nor the stored attachment identity.
void checkUnpublished(const ClientAttachment& attachment)
{
    CHECK(!attachment.held());
    CHECK(attachment.clientInstance() == initialClientInstance);
    CHECK(!attachment.paneTerminal(firstPaneID));
    CHECK(!attachment.paneTerminal(secondPaneID));
    CHECK(!attachment.restoredPictureGeneration(firstPaneID));
    CHECK(!attachment.restoredPictureGeneration(secondPaneID));
}

} // namespace

TEST(coldRestoreRefusesOneByteBelowItsDecodedCellAllowanceBeforeDestinationLookup)
{
    Ubiquitty::Terminal source;
    paint(source, 7, 23, 'A');
    auto record = emptyRecord();
    appendPicture(record, source, firstPaneID);
    REQUIRE(record.panes.size() == 1);
    REQUIRE(checkpointCellStorageBytes(record.panes[0].checkpoint) == decodedCellBytes);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    const auto before = UbiquittyTest::checkpointBody(frontend.first);
    ClientAttachment attachment(policyFor(decodedCellBytes - 1), measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    CHECK(!attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 0u);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == before);
    checkUnpublished(attachment);
    CHECK(store.read().has_value());
}

TEST(coldRestoreAdmitsItsExactDecodedCellBoundary)
{
    Ubiquitty::Terminal source;
    paint(source, 7, 23, 'A');
    auto record = emptyRecord();
    appendPicture(record, source, firstPaneID);
    REQUIRE(record.panes.size() == 1);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    ClientAttachment attachment(policyFor(decodedCellBytes), measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    REQUIRE(attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 1u);
    CHECK(attachment.paneTerminal(firstPaneID) == &frontend.first);
    CHECK(attachment.clientInstance() == record.clientInstance);
    CHECK(attachment.restoredPictureGeneration(firstPaneID) == storedGeneration);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == UbiquittyTest::checkpointBody(source));
    const auto held = attachment.held();
    REQUIRE(held);
    CHECK(held->hostInstanceID == record.hostInstanceID);
    CHECK(held->attachmentID == record.attachmentID);
}

TEST(aZeroColdRestoreCellAllowanceDoesNotMeanUnlimited)
{
    Ubiquitty::Terminal source;
    paint(source, 7, 23, 'A');
    auto record = emptyRecord();
    appendPicture(record, source, firstPaneID);
    REQUIRE(record.panes.size() == 1);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    const auto before = UbiquittyTest::checkpointBody(frontend.first);
    ClientAttachment attachment(policyFor(0), measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    CHECK(!attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 0u);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == before);
    checkUnpublished(attachment);
}

TEST(anOversizedSecondStoredPictureCannotPartiallyPublishTheFirst)
{
    Ubiquitty::Terminal small;
    Ubiquitty::Terminal large;
    paint(small, 5, 17, 'A');
    paint(large, 7, 23, 'K');
    auto record = emptyRecord();
    appendPicture(record, small, firstPaneID);
    appendPicture(record, large, secondPaneID);
    REQUIRE(record.panes.size() == 2);
    const auto smallBytes = checkpointCellStorageBytes(record.panes[0].checkpoint);
    REQUIRE(smallBytes);
    REQUIRE(*smallBytes < decodedCellBytes - 1);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    const auto beforeFirst = UbiquittyTest::checkpointBody(frontend.first);
    const auto beforeSecond = UbiquittyTest::checkpointBody(frontend.second);
    ClientAttachment attachment(policyFor(decodedCellBytes - 1), measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    CHECK(!attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 0u);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == beforeFirst);
    CHECK(UbiquittyTest::checkpointBody(frontend.second) == beforeSecond);
    checkUnpublished(attachment);
}

TEST(theColdRestoreCellAllowanceAppliesPerPictureRatherThanTheirSum)
{
    Ubiquitty::Terminal first;
    Ubiquitty::Terminal second;
    paint(first, 7, 23, 'A');
    paint(second, 7, 23, 'K');
    auto record = emptyRecord();
    appendPicture(record, first, firstPaneID);
    appendPicture(record, second, secondPaneID);
    REQUIRE(record.panes.size() == 2);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    ClientAttachment attachment(policyFor(decodedCellBytes), measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    REQUIRE(attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 2u);
    CHECK(attachment.paneTerminal(firstPaneID) == &frontend.first);
    CHECK(attachment.paneTerminal(secondPaneID) == &frontend.second);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == UbiquittyTest::checkpointBody(first));
    CHECK(UbiquittyTest::checkpointBody(frontend.second) == UbiquittyTest::checkpointBody(second));
}

TEST(coldRestoreStillEnforcesItsSeparateAggregateCellAllowance)
{
    Ubiquitty::Terminal source;
    paint(source, 7, 23, 'A');
    auto record = emptyRecord();
    appendPicture(record, source, firstPaneID);
    appendPicture(record, source, secondPaneID);
    REQUIRE(record.panes.size() == 2);
    auto policy = policyFor(decodedCellBytes);
    policy.maxCheckpointBytes = 2 * decodedCellBytes - 1;
    REQUIRE(pictureBytes(record) < policy.maxCheckpointBytes);
    MemoryResumeRecordStore store;
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    LentPictures frontend;
    const auto beforeFirst = UbiquittyTest::checkpointBody(frontend.first);
    const auto beforeSecond = UbiquittyTest::checkpointBody(frontend.second);
    ClientAttachment attachment(policy, measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);

    CHECK(!attachment.restoreFromStore());
    CHECK_EQ(frontend.lookups, 0u);
    CHECK(UbiquittyTest::checkpointBody(frontend.first) == beforeFirst);
    CHECK(UbiquittyTest::checkpointBody(frontend.second) == beforeSecond);
    checkUnpublished(attachment);
}

TEST(coldRestoreAdmitsEveryPictureAgainstTheReplicaAllowanceOrNone)
{
    Ubiquitty::Terminal source;
    paint(source, 7, 23, 'A');
    auto record = emptyRecord();
    appendPicture(record, source, firstPaneID);
    appendPicture(record, source, secondPaneID);
    REQUIRE(record.panes.size() == 2);
    // What the two destinations will retain once grown to the stored geometry.
    std::size_t retained = 0;
    {
        LentPictures probe;
        const auto first = probe.first.grid->backingStoragePlan(7, 23);
        const auto second = probe.second.grid->backingStoragePlan(7, 23);
        REQUIRE(first && second);
        retained = first->retainedBytes + second->retainedBytes;
    }
    for (const bool isRoom : { false, true }) {
        MemoryResumeRecordStore store;
        REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
        LentPictures frontend;
        const auto beforeFirst = UbiquittyTest::checkpointBody(frontend.first);
        auto policy = policyFor(decodedCellBytes);
        policy.maxRetainedReplicaBytes = isRoom ? retained : retained - 1;
        ClientAttachment attachment(policy, measuredLivenessPolicy(), { .clientInstance = initialClientInstance }, &frontend, &store);
        CHECK_EQ(attachment.restoreFromStore(), isRoom);
        if (isRoom) {
            CHECK(attachment.paneTerminal(firstPaneID) == &frontend.first);
            CHECK(attachment.paneTerminal(secondPaneID) == &frontend.second);
        } else {
            // Neither picture is published when both do not fit.
            CHECK(UbiquittyTest::checkpointBody(frontend.first) == beforeFirst);
            checkUnpublished(attachment);
        }
    }
}

UBIQUITTY_TEST_MAIN()
