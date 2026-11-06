// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Fallible, exact-capacity owners used by checkpoint preparation.
// Every allocation observation is synchronous and belongs to this binary.

#include "CheckpointContentFixture.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "TerminalFixture.h"

#include "Base/FallibleVector.h"
#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Document.h"
#include "Core/RunTextStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace {

struct AllocationObservation {
    std::size_t refusedOrdinal = 0;
    std::atomic<std::size_t> attempts { 0 };
    std::atomic<std::size_t> refusals { 0 };
    std::atomic<std::size_t> requestedBytes { 0 };
};

enum class AllocationForm {
    Object,
    Array
};

class ScopedAllocationObservation {
public:
    explicit ScopedAllocationObservation(AllocationObservation& observation)
        : m_previous(active().exchange(&observation))
    {
    }

    ~ScopedAllocationObservation() { active().store(m_previous); }
    ScopedAllocationObservation(const ScopedAllocationObservation&) = delete;
    ScopedAllocationObservation& operator=(const ScopedAllocationObservation&) = delete;

    static void* allocate(std::size_t bytes, AllocationForm form)
    {
        if (auto* observation = active().load()) {
            if (++observation->attempts == observation->refusedOrdinal) {
                ++observation->refusals;
                return nullptr;
            }
            observation->requestedBytes += bytes;
        }
        return form == AllocationForm::Array ? ::operator new[](bytes) : ::operator new(bytes);
    }

private:
    static std::atomic<AllocationObservation*>& active()
    {
        static std::atomic<AllocationObservation*> observation { nullptr };
        return observation;
    }

    AllocationObservation* m_previous;
};

Ubiquitty::RichText::Document document(std::uint32_t first, std::size_t count)
{
    Ubiquitty::RichText::Document value;
    value.text.assign(count, first);
    value.runs.push_back({ .length = static_cast<std::uint32_t>(count) });
    return value;
}

Ubiquitty::RichText::Block block(std::uint32_t serial)
{
    Ubiquitty::RichText::Block value;
    value.serial = serial;
    value.width = 7;
    value.height = 2;
    value.document = document('a', 3);
    value.visibleRects.push_back({ .width = 3, .height = 1 });
    return value;
}

std::unique_ptr<Ubiquitty::Terminal> terminal(int rows = 7, int columns = 23)
{
    auto value = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(value.get(), 64);
    Ubiquitty::resize(*value, rows, columns);
    return value;
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

TEST(decodedSparseRunTextsOwnExactlyTheirAdmittedSlotsAndText)
{
    Ubiquitty::RunTextStore store;
    REQUIRE(store.reserveDecoded(9));
    const std::array<std::uint32_t, 3> text { 'r', 'u', 'n' };
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        REQUIRE(store.tryAppendDecoded(9, text));
    }
    CHECK(observation.attempts.load() == 1);
    REQUIRE(store.slots().size() == 9);
    CHECK(store.slots()[8].capacity() == text.size());
    CHECK(store.storageBytes() == 9 * (sizeof(Ubiquitty::FallibleVector<std::uint32_t>) + sizeof(std::uint16_t)) + sizeof(text));
    const auto continuation = store.continuation();
    REQUIRE(continuation.freeIDs.size() == 8);
    for (std::size_t index = 0; index < continuation.freeIDs.size(); ++index)
        CHECK(continuation.freeIDs[index] == index + 1);
    CHECK(store.intern(std::span(text).first(1)) == 8);
}

TEST(decodedRunTextCapacityRefusalIsTypedBeforeAnyArrayRequest)
{
    Ubiquitty::RunTextStore store;
    AllocationObservation observation;
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return store.reserveDecoded(static_cast<std::size_t>(Ubiquitty::RunTextStore::maxEntries) + 1);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == Ubiquitty::RunTextStore::Error::OverCapacity);
    CHECK(observation.attempts.load() == 0);
    CHECK(store.slots().empty());
    CHECK(store.storageBytes() == 0);
}

TEST(aFullRunTextIDSpaceRefusesBeforeCopyingText)
{
    Ubiquitty::RunTextStore store;
    REQUIRE(store.reserveDecoded(Ubiquitty::RunTextStore::maxEntries));
    const std::array<std::uint32_t, 1> text { 'a' };
    for (int index = 0; index < Ubiquitty::RunTextStore::maxEntries; ++index)
        REQUIRE(store.intern(text) == index + 1);
    REQUIRE(store.isSweepDue());
    const auto beforeBytes = store.storageBytes();
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        CHECK(store.intern(text) == 0);
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(store.storageBytes() == beforeBytes);
    CHECK(store.liveCodePoints() == static_cast<std::size_t>(Ubiquitty::RunTextStore::maxEntries));
    CHECK(std::ranges::equal(store.text(1), text));
    CHECK(std::ranges::equal(store.text(Ubiquitty::RunTextStore::maxEntries), text));
}

TEST(eitherDecodedRunTextIndexOwnerCanRefuseAndRetry)
{
    for (const std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 } }) {
        Ubiquitty::RunTextStore store;
        AllocationObservation observation { .refusedOrdinal = ordinal };
        const auto outcome = [&] {
            ScopedAllocationObservation scope(observation);
            return store.reserveDecoded(9);
        }();
        REQUIRE(!outcome);
        CHECK(outcome.error() == Ubiquitty::RunTextStore::Error::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal);
        CHECK(observation.refusals.load() == 1);
        CHECK(store.slots().empty());
        CHECK(store.continuation().freeIDs.empty());
        CHECK(store.liveCodePoints() == 0);
        REQUIRE(store.reserveDecoded(9));
    }
}

TEST(decodedRunTextRefusalPreservesExistingTextAndReusableIDOrder)
{
    Ubiquitty::RunTextStore store;
    REQUIRE(store.reserveDecoded(9));
    const std::array<std::uint32_t, 3> before { 'o', 'l', 'd' };
    const std::array<std::uint32_t, 3> after { 'n', 'e', 'w' };
    REQUIRE(store.tryAppendDecoded(5, before));
    const auto beforeBytes = store.storageBytes();
    const auto beforeSlots = store.slots().size();
    const auto freeIDs = store.continuation().freeIDs;
    REQUIRE(freeIDs.size() == 4);
    const std::array<std::uint16_t, 4> beforeIDs { freeIDs[0], freeIDs[1], freeIDs[2], freeIDs[3] };
    AllocationObservation observation { .refusedOrdinal = 1 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return store.tryAppendDecoded(9, after);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == Ubiquitty::RunTextStore::Error::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    CHECK(store.storageBytes() == beforeBytes);
    CHECK(store.slots().size() == beforeSlots);
    CHECK(store.liveCodePoints() == before.size());
    CHECK(std::ranges::equal(store.text(5), before));
    CHECK(store.text(9).empty());
    CHECK(std::ranges::equal(store.continuation().freeIDs, beforeIDs));
    REQUIRE(store.tryAppendDecoded(9, after));
    CHECK(std::ranges::equal(store.text(9), after));
}

TEST(documentArrayRefusalPreservesTheCompletePreviousDocument)
{
    auto source = document('n', 11);
    REQUIRE(source.fonts.tryResize(2));
    source.fonts[0].name[0] = 'n';
    source.fonts[0].nameLength = 1;
    for (const std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 }, std::size_t { 3 } }) {
        auto destination = document('o', 3);
        REQUIRE(destination.fonts.tryResize(1));
        destination.fonts[0].name[0] = 'x';
        destination.fonts[0].nameLength = 1;
        const auto before = destination;
        const auto beforeBytes = destination.storageBytes();
        AllocationObservation observation { .refusedOrdinal = ordinal };
        const auto outcome = [&] {
            ScopedAllocationObservation scope(observation);
            return destination.tryAssign(source);
        }();
        REQUIRE(!outcome);
        CHECK(outcome.error() == Ubiquitty::StorageError::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal);
        CHECK(observation.refusals.load() == 1);
        CHECK(destination == before);
        CHECK(destination.storageBytes() == beforeBytes);
        REQUIRE(destination.tryAssign(source));
        CHECK(destination == source);
        CHECK(destination.text.capacity() == source.text.size());
        CHECK(destination.runs.capacity() == source.runs.size());
        CHECK(destination.fonts.capacity() == source.fonts.size());
        CHECK(destination.storageBytes() == source.text.size() * sizeof(std::uint32_t) + source.runs.size() * sizeof(Ubiquitty::RichText::Run) + source.fonts.size() * sizeof(Ubiquitty::RichText::FontEntry));
    }
}

TEST(documentCapacityRefusalPrecedesBothArrayAllocations)
{
    auto source = document('n', static_cast<std::size_t>(Ubiquitty::RichText::maxCodePoints) + 1);
    auto destination = document('o', 3);
    const auto before = destination;
    AllocationObservation observation;
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return destination.tryAssign(source);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == Ubiquitty::StorageError::OverCapacity);
    CHECK(observation.attempts.load() == 0);
    CHECK(destination == before);
}

TEST(documentValueAccessorsReadTextAndRunsWithoutAllocating)
{
    auto source = document(0x1F600, 3);
    REQUIRE(source.fonts.tryResize(1));
    source.fonts[0].name[0] = 'x';
    source.fonts[0].nameLength = 1;
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        CHECK(source.codePointCount() == 3);
        CHECK(source.runCount() == 1);
        CHECK(source.codePointAt(0) == 0x1F600);
        CHECK(source.codePointAt(2) == 0x1F600);
        const auto run = source.runAt(0);
        CHECK(run.start == 0 && run.length == 3);
        CHECK(run.halfPoints == Ubiquitty::RichText::defaultHalfPoints);
        CHECK(source.fontCount() == 1);
        auto font = source.fontAt(0);
        CHECK(font.familyName() == "x");
        font.name[0] = 'y';
        CHECK(source.fontAt(0).familyName() == "x");
    }
    CHECK(observation.attempts.load() == 0);
}

TEST(fontTableCapacityIsChargedAndItsBoundPrecedesCopyAllocations)
{
    using namespace Ubiquitty::RichText;
    Document source;
    CHECK(source.fonts.capacity() == 0);
    CHECK(source.storageBytes() == 0);
    REQUIRE(source.fonts.tryResize(3));
    CHECK(source.storageBytes() == 3 * sizeof(FontEntry));
    REQUIRE(source.fonts.tryReserve(maxFonts));
    CHECK(source.fontCount() == 3);
    CHECK(source.storageBytes() == maxFonts * sizeof(FontEntry));
    CHECK(isWellFormed(source));
    REQUIRE(source.fonts.tryResize(maxFonts + 1));
    CHECK(!isWellFormed(source));
    auto destination = document('o', 3);
    const auto before = destination;
    AllocationObservation observation;
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return destination.tryAssign(source);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == Ubiquitty::StorageError::OverCapacity);
    CHECK(observation.attempts.load() == 0);
    CHECK(destination == before);
}

TEST(documentFontDecodingIsFallibleAndPlanningDoesNotAllocate)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = document('n', 11);
    REQUIRE(source.fonts.tryResize(2));
    source.fonts[0].name[0] = 'x';
    source.fonts[0].nameLength = 1;
    source.runs[0].font = 0;
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writeDocument(writer, source);
    AllocationObservation planning;
    const auto view = [&] {
        ScopedAllocationObservation scope(planning);
        WireReader reader(bytes);
        return readDocumentView(reader);
    }();
    REQUIRE(view);
    CHECK(view->fonts == 2);
    CHECK(planning.attempts.load() == 0);

    Ubiquitty::RichText::Document decoded;
    AllocationObservation observation { .refusedOrdinal = 3 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        WireReader reader(bytes);
        return tryReadDocument(reader, decoded);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == ApplyOutcome::AllocationRefused);
    CHECK(observation.attempts.load() == 3);
    CHECK(observation.refusals.load() == 1);
    WireReader reader(bytes);
    REQUIRE(tryReadDocument(reader, decoded));
    CHECK(reader.isAtEnd());
    CHECK(decoded == source);
    CHECK(decoded.fonts.capacity() == source.fonts.size());
}

TEST(checkpointPreparationChargesDefinitionAndPlacementFontArrays)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    auto richText = block(17);
    REQUIRE(richText.document.fonts.tryResize(3));
    richText.document.fonts[0].name[0] = 'x';
    richText.document.fonts[0].nameLength = 1;
    richText.document.runs[0].font = 0;
    REQUIRE(source->richTextBlocks.tryRestore(Ubiquitty::ScreenMode::Primary, std::move(richText)));
    const auto body = UbiquittyTest::checkpointBody(*source);
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    CHECK(plan->richTextFonts == 3);
    CHECK(plan->placedRichTextFonts == 3);
    auto destination = terminal();
    const auto cost = checkpointStorageCost(*plan, *destination);
    REQUIRE(cost);
    CHECK(!prepareCheckpoint(body, *destination, { .maxStorageCost = *cost - 1 }));
    AllocationObservation observation;
    const auto prepared = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body, *destination, { .maxStorageCost = *cost });
    }();
    REQUIRE(prepared);
    CHECK(observation.requestedBytes.load() == plan->preparationStorageCost + sizeof(Ubiquitty::Terminal));
}

TEST(preReservedRichTextBlocksAdoptEveryDocumentAndRectangleWithoutAllocating)
{
    Ubiquitty::RichText::BlockStore store;
    REQUIRE(store.tryReserve(Ubiquitty::ScreenMode::Primary, 2));
    auto owned = block(17);
    const auto* text = owned.document.text.data();
    const auto* runs = owned.document.runs.data();
    const auto* rectangles = owned.visibleRects.data();
    const auto blockBytes = owned.document.storageBytes() + owned.visibleRects.capacity() * sizeof(Ubiquitty::RichText::VisibleRect);
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        REQUIRE(store.tryRestore(Ubiquitty::ScreenMode::Primary, std::move(owned)));
    }
    CHECK(observation.attempts.load() == 0);
    REQUIRE(store.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
    const auto& held = store.blocks(Ubiquitty::ScreenMode::Primary).front();
    CHECK(held.document.text.data() == text);
    CHECK(held.document.runs.data() == runs);
    CHECK(held.visibleRects.data() == rectangles);
    CHECK(store.storageBytes() == 2 * sizeof(Ubiquitty::RichText::Block) + blockBytes);
    CHECK(store.nextSerial() == 18);
}

TEST(richTextBlockArrayRefusalPreservesPlacementsAndSerials)
{
    Ubiquitty::RichText::BlockStore store;
    REQUIRE(store.tryReserve(Ubiquitty::ScreenMode::Primary, 1));
    REQUIRE(store.tryRestore(Ubiquitty::ScreenMode::Primary, block(17)));
    const auto beforeBytes = store.storageBytes();
    const auto* beforeText = store.blocks(Ubiquitty::ScreenMode::Primary).front().document.text.data();
    AllocationObservation observation { .refusedOrdinal = 1 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return store.tryReserve(Ubiquitty::ScreenMode::Primary, 2);
    }();
    REQUIRE(!outcome);
    CHECK(outcome.error() == Ubiquitty::StorageError::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    REQUIRE(store.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
    CHECK(store.blocks(Ubiquitty::ScreenMode::Primary).front().document.text.data() == beforeText);
    CHECK(store.storageBytes() == beforeBytes);
    CHECK(store.nextSerial() == 18);
    REQUIRE(store.tryReserve(Ubiquitty::ScreenMode::Primary, 2));
}

TEST(checkpointPreparationAccountsForEveryGrantedOwnerRequest)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    const auto body = UbiquittyTest::checkpointBody(*source);
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    REQUIRE(plan->linkCount && plan->clusterCount && plan->runTextSlots == 9);
    REQUIRE(plan->richTextDefinitionCount == 1 && plan->placedRichTextRectangles == 1);
    REQUIRE(plan->activeContexts == 1 && plan->hasEndedContext && plan->statusRecords == 1);
    REQUIRE(plan->hasStatusRow && plan->hasUserDefinedKeys);
    auto destination = terminal();
    const auto cost = checkpointStorageCost(*plan, *destination);
    REQUIRE(cost);
    AllocationObservation observation;
    const auto prepared = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body, *destination, { .maxStorageCost = *cost });
    }();
    REQUIRE(prepared);
    CHECK(observation.attempts.load() > 20);
    // Validation's scratch owner is released before materialization starts.
    // The capacity allowance charges that sequential owner only once.
    CHECK(observation.requestedBytes.load() == plan->preparationStorageCost + sizeof(Ubiquitty::Terminal));
}

TEST(everyCheckpointPreparationOwnerCanRefuseWithoutChangingTheDestination)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    const auto body = UbiquittyTest::checkpointBody(*source);
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    auto destination = terminal();
    UbiquittyTest::feed(*destination, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven\r\neight\r\nold");
    const auto before = UbiquittyTest::checkpointBody(*destination);
    const auto history = Ubiquitty::Screen::scrollbackCount(destination.get());
    REQUIRE(history > 0);
    const auto cost = checkpointStorageCost(*plan, *destination);
    REQUIRE(cost);
    AllocationObservation measurement;
    {
        ScopedAllocationObservation scope(measurement);
        REQUIRE(prepareCheckpoint(body, *destination, { .maxStorageCost = *cost }));
    }
    REQUIRE(measurement.attempts.load() > 20);
    for (std::size_t ordinal = 1; ordinal <= measurement.attempts.load(); ++ordinal) {
        AllocationObservation observation { .refusedOrdinal = ordinal };
        const auto prepared = [&] {
            ScopedAllocationObservation scope(observation);
            return prepareCheckpoint(body, *destination, { .maxStorageCost = *cost });
        }();
        REQUIRE(!prepared);
        CHECK(prepared.error() == ApplyOutcome::AllocationRefused);
        CHECK(observation.refusals.load() == 1);
        CHECK(UbiquittyTest::checkpointBody(*destination) == before);
        CHECK(Ubiquitty::Screen::scrollbackCount(destination.get()) == history);
    }
    REQUIRE(prepareCheckpoint(body, *destination, { .maxStorageCost = *cost }));
}

TEST(fullCheckpointCapacityBoundaryIsExactAndIndependentOfItsCellAllowance)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    REQUIRE(UbiquittyTest::seedCheckpointOwners(*source));
    auto body = UbiquittyTest::checkpointBody(*source);
    const auto expected = body;
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    auto destination = terminal();
    const auto before = UbiquittyTest::checkpointBody(*destination);
    const auto cost = checkpointStorageCost(*plan, *destination);
    REQUIRE(cost && *cost > plan->screenCellBytes);
    AllocationObservation observation;
    const auto refused = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body, *destination, { .maxDecodedScreenCellBytes = plan->screenCellBytes, .maxStorageCost = *cost - 1 });
    }();
    REQUIRE(!refused);
    CHECK(refused.error() == ApplyOutcome::AllocationRefused);
    CHECK(observation.attempts.load() == 1);
    CHECK(observation.requestedBytes.load() == sizeof(Ubiquitty::Terminal));
    CHECK(UbiquittyTest::checkpointBody(*destination) == before);
    auto prepared = prepareCheckpoint(body, *destination, { .maxDecodedScreenCellBytes = plan->screenCellBytes, .maxStorageCost = *cost });
    REQUIRE(prepared);
    std::fill(body.begin(), body.end(), 0xA5);
    std::vector<std::uint8_t>().swap(body);
    ScreenReplica replica;
    REQUIRE(commitCheckpoint(*destination, std::move(*prepared), replica) == ApplyOutcome::Applied);
    CHECK(UbiquittyTest::checkpointBody(*destination) == expected);
}

TEST(aZeroFullCheckpointAllowanceRefusesBeforeFixedScratchAllocation)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    const auto body = UbiquittyTest::checkpointBody(*source);
    auto destination = terminal();
    AllocationObservation observation;
    const auto prepared = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body, *destination, { .maxStorageCost = 0 });
    }();
    REQUIRE(!prepared);
    CHECK(prepared.error() == ApplyOutcome::AllocationRefused);
    CHECK(observation.attempts.load() == 0);
}

TEST(checkpointDestinationCostAdditionRefusesOverflowAndMissingBacking)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto source = terminal();
    const auto body = UbiquittyTest::checkpointBody(*source);
    auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    auto destination = terminal();
    plan->preparationStorageCost = std::numeric_limits<std::size_t>::max();
    const auto overflow = checkpointStorageCost(*plan, *destination);
    REQUIRE(!overflow);
    CHECK(overflow.error() == ApplyOutcome::AllocationRefused);
    Ubiquitty::Terminal stateOnly(Ubiquitty::TerminalInitialization::StateOnly);
    REQUIRE(!stateOnly.grid);
    const auto noBacking = checkpointStorageCost(*plan, stateOnly);
    REQUIRE(!noBacking);
    CHECK(noBacking.error() == ApplyOutcome::AllocationRefused);
}

UBIQUITTY_TEST_MAIN()
