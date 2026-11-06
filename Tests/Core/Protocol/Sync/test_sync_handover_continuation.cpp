// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "TerminalFixture.h"

#include "Core/LineStore.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

std::unique_ptr<Terminal> terminal()
{
    auto result = std::make_unique<Terminal>();
    resize(*result, 6, 40);
    result->drawsSizedText = true;
    result->drawsProportionalText = true;
    return result;
}

void restoreHistory(const Terminal& source, Terminal& target)
{
    target.historyEpoch = source.historyEpoch;
    target.historyEpochBase = source.historyEpochBase;
    if (!source.lineStore)
        return;
    std::vector<std::vector<Cell>> cells;
    std::vector<StoredLine> rows;
    for (int index = 0; index < source.lineStore->count(); ++index) {
        const auto& row = *source.lineStore->at(index);
        cells.emplace_back();
        if (row.length)
            cells.back().assign(row.cells, row.cells + row.length);
        rows.push_back(row);
        rows.back().cells = cells.back().data();
    }
    auto store = std::make_unique<LineStore>();
    store->init(source.lineStore->capacity());
    REQUIRE(store->restore(rows, source.lineStore->continuation()));
    target.lineStore = std::move(store);
    target.scrollbackCapacity = target.lineStore->capacity();
    target.scrollbackCount = target.lineStore->count();
    target.scrollbackLimit = source.scrollbackLimit;
}

} // namespace

TEST(aRestoredPublisherContinuesAnExistingReplicasPenIDs)
{
    auto source = terminal();
    ScreenPublisher original;
    UbiquittyTest::feed(*source, "\x1b[31mretired red");
    REQUIRE(original.capture(*source));
    UbiquittyTest::feed(*source, "\x1b[H\x1b[2J\x1b[32mgreen\x1b[0m"
                                 "\x1b]8;id=kept;https://example.invalid/\x1b\\link\x1b]8;;\x1b\\"
                                 "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"
                                 "\x1b[4;1H\x1b]66;p=1:c=12;full run text\x1b\\");
    REQUIRE(original.capture(*source));
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    REQUIRE(original.checkpoint(*source, 71, checkpoint, baseline));
    auto snapshot = original.continuation();
    REQUIRE(snapshot);
    auto successor = terminal();
    ScreenReplica unused;
    REQUIRE(applyCheckpoint(*successor, checkpoint, unused) == ApplyOutcome::Applied);
    restoreHistory(*source, *successor);

    ScreenPublisher renumbered;
    REQUIRE(renumbered.capture(*successor));
    CHECK(renumbered.rows(0).hash(0) != original.rows(0).hash(0));
    ScreenPublisher restored;
    REQUIRE(restored.restore(*successor, *snapshot));
    for (int screen = 0; screen < 2; ++screen)
        for (int row = 0; row < source->rows; ++row)
            CHECK_EQ(restored.rows(screen).hash(row), original.rows(screen).hash(row));

    auto receiver = terminal();
    ScreenReplica held;
    REQUIRE(applyCheckpoint(*receiver, checkpoint, held) == ApplyOutcome::Applied);
    for (const auto change : { "\x1b[2;4H\x1b[35mnext", "\x1b[?1049h\x1b[33malternate", "\x1b[?1049l\x1b[1;2H!" }) {
        UbiquittyTest::feed(*source, change);
        UbiquittyTest::feed(*successor, change);
        REQUIRE(original.capture(*source));
        REQUIRE(restored.capture(*successor));
        auto expectedBaseline = baseline;
        std::vector<std::uint8_t> expected;
        std::vector<std::uint8_t> actual;
        REQUIRE(original.rowUpdate(*source, expectedBaseline, expected) == UpdateOutcome::Encoded);
        REQUIRE(restored.rowUpdate(*successor, baseline, actual) == UpdateOutcome::Encoded);
        CHECK(actual == expected);
        REQUIRE(applyRowUpdate(*receiver, actual, held) == ApplyOutcome::Applied);
        CHECK(UbiquittyTest::firstDisagreement(*source, *receiver).empty());
    }
}

TEST(dictionaryRestoreRetainsEpochMarksAndRejectsDuplicateDefinitions)
{
    PaneDictionaries original;
    original.roll();
    Pen first {};
    first.foregroundIndex = 2;
    CHECK_EQ(original.penID(first), 0u);
    original.noteCheckpointed();
    Pen second {};
    second.foregroundIndex = 4;
    CHECK_EQ(original.penID(second), 1u);
    auto snapshot = original.snapshot();
    auto restored = PaneDictionaries::restored(std::move(snapshot));
    REQUIRE(restored);
    CHECK_EQ(restored->epoch(), original.epoch());
    CHECK_EQ(restored->penID(second), 1u);
    CHECK_EQ(restored->penID(first), 0u);
    CHECK(restored->snapshot().checkpointed == original.snapshot().checkpointed);
    snapshot = original.snapshot();
    snapshot.pens.push_back(first);
    CHECK(!PaneDictionaries::restored(std::move(snapshot)));
    snapshot = original.snapshot();
    snapshot.checkpointed.links = 1;
    CHECK(!PaneDictionaries::restored(std::move(snapshot)));
    snapshot = original.snapshot();
    snapshot.links.push_back({ .id = 0, .key = {}, .uri = "https://example.invalid/" });
    CHECK(!PaneDictionaries::restored(std::move(snapshot)));
}

TEST(aMismatchingPublisherRestoreLeavesItsExistingCaptureUsable)
{
    auto source = terminal();
    UbiquittyTest::feed(*source, "source");
    ScreenPublisher publisher;
    REQUIRE(publisher.capture(*source));
    auto snapshot = publisher.continuation();
    REQUIRE(snapshot);
    const auto hash = publisher.rows(0).hash(0);
    for (int fault = 0; fault < 5; ++fault) {
        auto damaged = *snapshot;
        switch (fault) {
        case 0:
            ++damaged.columns;
            break;
        case 1:
            ++damaged.rowHashes[0][0];
            break;
        case 2:
            ++damaged.blockHashes[0];
            break;
        case 3:
            ++damaged.history.head;
            break;
        case 4:
            damaged.rowHashes[1].clear();
            break;
        }
        CHECK(!publisher.restore(*source, std::move(damaged)));
        CHECK(publisher.isCaptured());
        CHECK_EQ(publisher.rows(0).hash(0), hash);
    }
    ScreenPublisher empty;
    CHECK(!empty.continuation());
}

TEST(historyRewriteIdentitySurvivesPullbackAndReconstruction)
{
    auto source = terminal();
    source->lineStore = std::make_unique<LineStore>();
    source->lineStore->init(20);
    Cell cell = defaultBlankCell;
    HistoryPublisher original;
    for (int index = 0; index < 8; ++index) {
        cell.codePoint = 'a' + index;
        source->lineStore->append(&cell, 1, 0, 0, false, false);
    }
    REQUIRE(original.capture(*source));
    source->lineStore->dropNewest(2);
    REQUIRE(original.capture(*source));
    auto successor = terminal();
    restoreHistory(*source, *successor);
    auto snapshot = original.continuation();
    REQUIRE(snapshot);
    REQUIRE(original.publishedHash(7));
    HistoryPublisher restored;
    REQUIRE(restored.restore(*successor, *snapshot));
    CHECK(restored.publishedHash(7) == original.publishedHash(7));
    cell.codePoint = 'Z';
    source->lineStore->append(&cell, 1, 0, 0, false, false);
    successor->lineStore->append(&cell, 1, 0, 0, false, false);
    REQUIRE(original.capture(*source));
    REQUIRE(restored.capture(*successor));
    CHECK_EQ(restored.revision(), 1u);
    CHECK_EQ(restored.revision(), original.revision());
    CHECK(restored.rewrittenSince(0, 8) == std::vector<std::uint64_t> { 6 });
    CHECK(restored.rewrittenSince(0, 8) == original.rewrittenSince(0, 8));
    std::vector<std::uint8_t> expected;
    std::vector<std::uint8_t> actual;
    CHECK_EQ(original.writeBody(*source, { 0, 7 }, HistoryOrder::Ascending, maxFrameBodyBytes, expected), 7u);
    CHECK_EQ(restored.writeBody(*successor, { 0, 7 }, HistoryOrder::Ascending, maxFrameBodyBytes, actual), 7u);
    CHECK(actual == expected);
    snapshot = restored.continuation();
    REQUIRE(snapshot);
    HistoryPublisher secondSuccessor;
    REQUIRE(secondSuccessor.restore(*successor, *snapshot));
    CHECK_EQ(secondSuccessor.revision(), 1u);
    CHECK(secondSuccessor.rewrittenSince(0, 8) == restored.rewrittenSince(0, 8));
    auto damaged = *snapshot;
    damaged.hashes.back().revision = 2;
    CHECK(!secondSuccessor.restore(*successor, std::move(damaged)));
    damaged = *snapshot;
    ++damaged.hashes.front().hash;
    CHECK(!secondSuccessor.restore(*successor, std::move(damaged)));
    damaged = *snapshot;
    damaged.revision = std::numeric_limits<std::uint64_t>::max();
    CHECK(!secondSuccessor.restore(*successor, std::move(damaged)));
    CHECK_EQ(secondSuccessor.revision(), 1u);
}

TEST(aSuccessorsPublicationFollowsThePredecessorsGeneration)
{
    auto source = terminal();
    const ID128 paneID { 12, 34 };
    PaneSource original(*source, paneID);
    UbiquittyTest::feed(*source, "\x1b[31mold colour");
    original.publish(FrameClass::Structural, 100);
    UbiquittyTest::feed(*source, "\x1b[H\x1b[2J\x1b[32mcurrent\x1b[0m");
    original.publish(FrameClass::Settled, 200);
    original.noteTerminalChanged();
    CHECK(!original.publicationContinuation());
    original.publish(FrameClass::Boundary, 300);
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    REQUIRE(original.publisher().checkpoint(*source, original.generation(), checkpoint, baseline));
    auto continuation = original.publicationContinuation();
    REQUIRE(continuation);
    auto successor = terminal();
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*successor, checkpoint, replica) == ApplyOutcome::Applied);
    restoreHistory(*source, *successor);
    PaneSource restored(*successor, paneID);
    REQUIRE(restored.restorePublication(*continuation));
    CHECK_EQ(restored.generation(), original.generation());
    CHECK_EQ(restored.structuralGeneration(), original.structuralGeneration());
    CHECK_EQ(restored.changes(), original.changes());
    CHECK(restored.frameClass() == FrameClass::Boundary);
    CHECK(!restored.restorePublication(*continuation));
    UbiquittyTest::feed(*source, "\x1b[2;1Hnext");
    UbiquittyTest::feed(*successor, "\x1b[2;1Hnext");
    original.publish(FrameClass::Settled, 400);
    restored.publish(FrameClass::Settled, 400);
    CHECK_EQ(restored.generation(), continuation->generation + 1);
    CHECK_EQ(restored.generation(), original.generation());
    CHECK_EQ(restored.publicationContinuation()->modesRevision, original.publicationContinuation()->modesRevision);
    std::vector<std::uint8_t> expected;
    std::vector<std::uint8_t> actual;
    auto expectedBaseline = baseline;
    REQUIRE(original.publisher().rowUpdate(*source, expectedBaseline, expected) == UpdateOutcome::Encoded);
    REQUIRE(restored.publisher().rowUpdate(*successor, baseline, actual) == UpdateOutcome::Encoded);
    CHECK(actual == expected);
}

TEST(aPublicationRestoreRejectsWrongPaneAndInvalidCountersWithoutActivation)
{
    auto source = terminal();
    PaneSource original(*source, { 12, 34 });
    original.publish(FrameClass::Settled, 100);
    auto continuation = original.publicationContinuation();
    REQUIRE(continuation);
    REQUIRE(continuation->modesRevision != 0);
    for (int fault = 0; fault < 10; ++fault) {
        PaneSource restored(*source, { 12, 34 });
        auto damaged = *continuation;
        switch (fault) {
        case 0:
            ++damaged.paneID.low;
            break;
        case 1:
            damaged.generation = 0;
            break;
        case 2:
            damaged.structuralGeneration = damaged.generation + 1;
            break;
        case 3:
            damaged.changes = 0;
            break;
        case 4:
            damaged.frameClass = static_cast<FrameClass>(255);
            break;
        case 5:
            damaged.publishedModesHash ^= 1;
            break;
        case 6:
            damaged.modesRevision = std::numeric_limits<std::uint32_t>::max();
            break;
        case 7:
            damaged.modesRevision = 0;
            break;
        case 8:
            damaged.generation = std::numeric_limits<std::uint64_t>::max();
            damaged.changes = damaged.generation;
            break;
        case 9:
            damaged.changes = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        CHECK(!restored.restorePublication(std::move(damaged)));
        CHECK_EQ(restored.generation(), 0u);
        CHECK(!restored.isFrameHeld());
    }
}

UBIQUITTY_TEST_MAIN()
