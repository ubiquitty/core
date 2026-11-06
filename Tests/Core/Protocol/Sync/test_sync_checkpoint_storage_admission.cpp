// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Component-only admission and ownership for decoded checkpoint cells.
// These controls observe nothrow allocations, not every decoder container.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "TerminalFixture.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/HistoryLines.h"
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

using namespace Ubiquitty::Protocol::Sync;

enum class AllocationForm {
    Object,
    Array
};

struct AllocationObservation {
    std::size_t screenArrayBytes = 0;
    std::size_t refusedScreenOrdinal = 0;
    bool refusesStatusOwner = false;
    std::size_t refusedAttemptOrdinal = 0;
    std::size_t rowAttributeArrayBytes = 0;
    std::size_t refusedAttributeOrdinal = 0;
    bool refusesLinkTableOwner = false;
    bool refusesUserDefinedKeysOwner = false;
    std::atomic<std::size_t> attempts { 0 };
    std::atomic<std::size_t> screenArrays { 0 };
    std::atomic<std::size_t> statusOwners { 0 };
    std::atomic<std::size_t> attributeArrays { 0 };
    std::atomic<std::size_t> linkTableOwners { 0 };
    std::atomic<std::size_t> userDefinedKeysOwners { 0 };
    std::atomic<std::size_t> refusals { 0 };
    std::atomic<void*> statusOwner { nullptr };
    std::atomic<void*> linkTableOwner { nullptr };
};

// The policy owner lives for the binary, while each observation belongs to
// a synchronous test scope. No allocator callback allocates logging state.
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
        AllocationObservation* observation = active().load();
        const bool isStatusOwner = form == AllocationForm::Object && bytes == sizeof(Ubiquitty::Rendering::StatusRowStorage);
        const bool isLinkTableOwner = form == AllocationForm::Object && bytes == sizeof(Ubiquitty::Attributes::Hyperlinks::LinkTable);
        const bool isUserDefinedKeysOwner = form == AllocationForm::Object && bytes == sizeof(Ubiquitty::VT::UserDefinedKeys);
        if (observation) {
            const auto attempt = ++observation->attempts;
            bool refuses = attempt == observation->refusedAttemptOrdinal;
            if (form == AllocationForm::Array && bytes == observation->screenArrayBytes) {
                const auto ordinal = ++observation->screenArrays;
                refuses = refuses || ordinal == observation->refusedScreenOrdinal;
            }
            if (isStatusOwner) {
                ++observation->statusOwners;
                refuses = refuses || observation->refusesStatusOwner;
            }
            if (form == AllocationForm::Array && bytes == observation->rowAttributeArrayBytes) {
                const auto ordinal = ++observation->attributeArrays;
                refuses = refuses || ordinal == observation->refusedAttributeOrdinal;
            }
            if (isLinkTableOwner) {
                ++observation->linkTableOwners;
                refuses = refuses || observation->refusesLinkTableOwner;
            }
            if (isUserDefinedKeysOwner) {
                ++observation->userDefinedKeysOwners;
                refuses = refuses || observation->refusesUserDefinedKeysOwner;
            }
            if (refuses) {
                ++observation->refusals;
                return nullptr;
            }
        }
        // Core deletes a granted nothrow allocation normally; keep the
        // ordinary allocation/deallocation pair rather than substituting malloc.
        void* result = form == AllocationForm::Array ? ::operator new[](bytes) : ::operator new(bytes);
        if (observation && isStatusOwner)
            observation->statusOwner.store(result);
        if (observation && isLinkTableOwner)
            observation->linkTableOwner.store(result);
        return result;
    }

private:
    static std::atomic<AllocationObservation*>& active()
    {
        static std::atomic<AllocationObservation*> observation { nullptr };
        return observation;
    }

    AllocationObservation* m_previous;
};

std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows = 7, int columns = 23)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

std::vector<std::uint8_t> checkpointOf(const Ubiquitty::Terminal& terminal)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(terminal) && publisher.checkpoint(terminal, 1, body, baseline));
    return body;
}

void setStatusRow(Ubiquitty::Terminal& terminal, std::uint32_t first, std::uint32_t last)
{
    terminal.statusLineType = 2;
    auto row = Ubiquitty::Rendering::ensureStatusRow(terminal);
    std::fill(row.begin(), row.end(), Ubiquitty::defaultBlankCell);
    row.front().codePoint = first;
    row.back().codePoint = last;
}

void checkReplicaUnchanged(const ScreenReplica& actual, const ScreenReplica& before)
{
    CHECK(actual.generation == before.generation);
    CHECK(actual.dictionaryEpoch == before.dictionaryEpoch);
    REQUIRE(actual.pens().size() == before.pens().size());
    if (!actual.pens().empty())
        CHECK(std::memcmp(actual.pens().data(), before.pens().data(), actual.pens().size() * sizeof(Ubiquitty::Pen)) == 0);
    CHECK(actual.maxPens == before.maxPens);
    CHECK(actual.storage == before.storage);
    CHECK(actual.blockHashes == before.blockHashes);
    CHECK(actual.historyEpoch == before.historyEpoch);
    CHECK(actual.historyHead == before.historyHead);
    CHECK(actual.stateDigest == before.stateDigest);
    CHECK(actual.scratch == before.scratch);
    CHECK(actual.echoContext == before.echoContext);
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

TEST(checkpointCellAllowanceRefusesBeforeDecodingOrAllocatingArrays)
{
    auto host = makeTerminal();
    const auto body = checkpointOf(*host);
    constexpr std::size_t cells = 2 * 7 * 23 * sizeof(Ubiquitty::Cell);
    CHECK(checkpointCellStorageBytes(body) == cells);
    // A header-only body would fail later validation. Its allowance refusal
    // therefore also witnesses that decoding has not reached the dictionaries.
    for (const auto bytes : { std::span<const std::uint8_t>(body), std::span<const std::uint8_t>(body).first(17) }) {
        AllocationObservation observation;
        auto prepared = [&] {
            ScopedAllocationObservation scope(observation);
            return prepareCheckpoint(bytes, cells - 1);
        }();
        REQUIRE(!prepared);
        CHECK(prepared.error() == ApplyOutcome::AllocationRefused);
        CHECK(observation.attempts.load() == 0);
    }
}

TEST(checkpointCellAllowanceAdmitsItsExactTwoArrayBoundary)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "primary\x1b[?1049halternate");
    auto body = checkpointOf(*host);
    constexpr std::size_t screenBytes = 7 * 23 * sizeof(Ubiquitty::Cell);
    AllocationObservation observation { .screenArrayBytes = screenBytes };
    auto prepared = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body, 2 * screenBytes);
    }();
    REQUIRE(prepared);
    CHECK(observation.screenArrays.load() == 2);
    CHECK(observation.refusals.load() == 0);
    CHECK(prepared->rows() == 7);
    CHECK(prepared->columns() == 23);
    std::fill(body.begin(), body.end(), 0xA5);
    std::vector<std::uint8_t>().swap(body);
    auto client = makeTerminal(3, 11);
    ScreenReplica replica;
    REQUIRE(commitCheckpoint(*client, std::move(*prepared), replica) == ApplyOutcome::Applied);
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(eitherCheckpointScreenArrayRefusalKeepsTerminalAndReplicaUnchanged)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "primary\x1b[?1049halternate");
    const auto body = checkpointOf(*host);
    auto client = makeTerminal(3, 11);
    UbiquittyTest::feed(*client, "before\r\none\r\ntwo\r\nthree\r\nfour");
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    UbiquittyTest::feed(*client, "\r\nkept history\r\n");
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const auto* beforeStatus = client->statusRow.get();
    const auto historyLines = Ubiquitty::Screen::scrollbackCount(client.get());
    REQUIRE(historyLines > 0);
    for (const std::size_t ordinal : { std::size_t { 1 }, std::size_t { 2 } }) {
        AllocationObservation observation { .screenArrayBytes = 7 * 23 * sizeof(Ubiquitty::Cell), .refusedScreenOrdinal = ordinal };
        ApplyOutcome outcome;
        {
            ScopedAllocationObservation scope(observation);
            outcome = applyCheckpoint(*client, body, replica);
        }
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK(observation.screenArrays.load() == ordinal);
        CHECK(observation.attempts.load() == ordinal + 1);
        CHECK(observation.refusals.load() == 1);
        CHECK(checkpointOf(*client) == before);
        CHECK(client->statusRow.get() == beforeStatus);
        CHECK(Ubiquitty::Screen::scrollbackCount(client.get()) == historyLines);
        checkReplicaUnchanged(replica, beforeReplica);
    }
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(checkpointStatusOwnerRefusalIsTypedAndTransactional)
{
    auto host = makeTerminal();
    setStatusRow(*host, 'H', 'T');
    const auto body = checkpointOf(*host);
    auto client = makeTerminal();
    UbiquittyTest::feed(*client, "previous picture");
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const auto* beforeStatus = client->statusRow.get();
    AllocationObservation observation { .refusesStatusOwner = true };
    ApplyOutcome outcome;
    {
        ScopedAllocationObservation scope(observation);
        outcome = applyCheckpoint(*client, body, replica);
    }
    CHECK(outcome == ApplyOutcome::AllocationRefused);
    CHECK(observation.statusOwners.load() == 1);
    CHECK(observation.refusals.load() == 1);
    CHECK(checkpointOf(*client) == before);
    CHECK(client->statusRow.get() == beforeStatus);
    checkReplicaUnchanged(replica, beforeReplica);
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    REQUIRE(client->statusRow);
    CHECK(client->statusRow->cells.back().codePoint == 'T');
}

TEST(checkpointStatusOwnerMovesAtFullWidth)
{
    auto host = makeTerminal();
    setStatusRow(*host, 'H', 'T');
    const auto body = checkpointOf(*host);
    AllocationObservation observation;
    auto prepared = [&] {
        ScopedAllocationObservation scope(observation);
        return prepareCheckpoint(body);
    }();
    REQUIRE(prepared);
    REQUIRE(observation.statusOwners.load() == 1);
    auto client = makeTerminal();
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(commitCheckpoint(*client, std::move(*prepared), replica) == ApplyOutcome::Applied);
    CHECK(client->statusRow.get() == observation.statusOwner.load());
    REQUIRE(client->statusRow);
    CHECK(std::memcmp(client->statusRow->cells.data(), host->statusRow->cells.data(), sizeof(Ubiquitty::Rendering::StatusRowStorage)) == 0);
    CHECK(client->statusRow->cells[Ubiquitty::maxColumns - 1].codePoint == 'T');
    // Absence is also owned state: committing it releases a previous row.
    Ubiquitty::Rendering::releaseStatusRow(*host);
    REQUIRE(applyCheckpoint(*client, checkpointOf(*host), replica) == ApplyOutcome::Applied);
    CHECK(!client->statusRow);
}

TEST(rowUpdateStatusOwnerRefusalIsTypedAndTransactional)
{
    auto host = makeTerminal();
    setStatusRow(*host, 'H', 'T');
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal();
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    // Establish the existing private update scratch owner before observing
    // refusal; only public terminal and replica state are transactional.
    body.clear();
    REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    REQUIRE(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    REQUIRE(replica.scratch);
    setStatusRow(*host, 'N', 'W');
    UbiquittyTest::feed(*host, "\x1b[3;5H");
    body.clear();
    REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const auto* beforeStatus = client->statusRow.get();
    AllocationObservation observation { .refusesStatusOwner = true };
    ApplyOutcome outcome;
    {
        ScopedAllocationObservation scope(observation);
        outcome = applyRowUpdate(*client, body, replica);
    }
    CHECK(outcome == ApplyOutcome::AllocationRefused);
    CHECK(observation.statusOwners.load() == 1);
    CHECK(observation.refusals.load() == 1);
    CHECK(checkpointOf(*client) == before);
    CHECK(client->statusRow.get() == beforeStatus);
    checkReplicaUnchanged(replica, beforeReplica);
    REQUIRE(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    REQUIRE(client->statusRow);
    CHECK(std::memcmp(client->statusRow->cells.data(), host->statusRow->cells.data(), sizeof(Ubiquitty::Rendering::StatusRowStorage)) == 0);
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(checkpointPlanningReportsExactArraysAndAllBorrowedOwnerCounts)
{
    auto host = makeTerminal();
    host->titleUTF8 = std::string(200, 'T');
    host->iconNameUTF8 = std::string(100, 'I');
    host->titleUTF8Stack[3] = std::string(150, 'S');
    const std::array<std::uint32_t, 3> cluster { 'a', 0x301, 0x302 };
    REQUIRE(host->clusterPool.appendDecoded(1, cluster));
    const std::array<std::uint32_t, 4> run { 'r', 'u', 'n', '!' };
    REQUIRE(host->runTextStore.intern(run) == 1);
    Ubiquitty::OSC::Context context;
    context.id = "command-1";
    context.fields[0] = "command";
    context.fields[static_cast<std::size_t>(Ubiquitty::OSC::ContextField::CommandLine)] = std::string(200, 'c');
    host->contexts.active.push_back(context);
    host->contexts.lastEnded = context;
    const Ubiquitty::OSC::ProgramStatus status { "job", "builder", "Building", std::string(900, 'm'),
        Ubiquitty::OSC::ProgramState::Working, Ubiquitty::OSC::ProgramBlockKind::None, 10 };
    host->programStatus.records.push_back(status);
    Ubiquitty::RichText::Block block;
    block.serial = 17;
    block.id = 1;
    block.width = 5;
    block.height = 1;
    block.document.text = { 'o', 'n', 'e' };
    block.document.runs.push_back({ .length = 3 });
    host->richTextBlocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    setStatusRow(*host, 'H', 'T');
    const auto body = checkpointOf(*host);
    AllocationObservation observation { .screenArrayBytes = 7 * 23 * sizeof(Ubiquitty::Cell), .refusesStatusOwner = true };
    const auto plan = [&] {
        ScopedAllocationObservation scope(observation);
        return checkpointStoragePlan(body);
    }();
    REQUIRE(plan);
    CHECK(plan->rows == 7 && plan->columns == 23);
    CHECK(plan->screenCellBytes == 2 * 7 * 23 * sizeof(Ubiquitty::Cell));
    CHECK(plan->rowAttributeBytes == 2 * 7 * sizeof(RowAttributes));
    CHECK(plan->rowHashBytes == 2 * 7 * sizeof(std::uint64_t));
    REQUIRE(ReplicaStorageBuilder::storageBytesForCapacity(plan->penCount, { 7, 7 }));
    CHECK(plan->replicaStorageBytes == *ReplicaStorageBuilder::storageBytesForCapacity(plan->penCount, { 7, 7 }));
    REQUIRE(Ubiquitty::ClusterPool::storageBytesForCapacity(1));
    CHECK(plan->clusterStorageBytes == *Ubiquitty::ClusterPool::storageBytesForCapacity(1));
    CHECK(plan->penCount >= 1);
    CHECK(plan->clusterCount == 1 && plan->clusterCodePoints == 3);
    CHECK(plan->runTextCount == 1 && plan->runTextSlots == 1 && plan->runTextCodePoints == 4);
    CHECK(plan->richTextDefinitionCount == 1 && plan->richTextCodePoints == 3 && plan->richTextRuns == 1);
    CHECK(plan->richTextBlocks[0] == 1 && plan->richTextBlocks[1] == 0);
    CHECK(plan->placedRichTextCodePoints == 3 && plan->placedRichTextRuns == 1);
    CHECK(plan->activeContexts == 1 && plan->hasEndedContext && plan->statusRecords == 1);
    CHECK(plan->stateStringBytes == 200 + 100 + 150 + 2 * (9 + 7 + 200) + 3 + 7 + 8 + 900);
    CHECK(plan->hasStatusRow);
    CHECK(plan->validationScratchBytes == sizeof(Ubiquitty::Terminal));
    // This observer sees nothrow calls only. It proves the absent screen/status
    // owners; the borrowed leaf implementation supplies the container contract.
    CHECK(observation.screenArrays.load() == 0);
    CHECK(observation.statusOwners.load() == 0);
    CHECK(observation.refusals.load() == 0);
    CHECK(checkpointOf(*host) == body);
    REQUIRE(prepareCheckpoint(body));
}

TEST(checkpointPlanningScratchDoesNotScaleWithCarriedGeometryOrText)
{
    auto small = makeTerminal(7, 23);
    auto large = makeTerminal(31, 129);
    large->titleUTF8 = std::string(1000, 'T');
    UbiquittyTest::feed(*large, "wide \xe7\x95\x8c and text\r\n");
    const auto smallBody = checkpointOf(*small);
    const auto largeBody = checkpointOf(*large);
    AllocationObservation smallObservation;
    const auto smallPlan = [&] {
        ScopedAllocationObservation scope(smallObservation);
        return checkpointStoragePlan(smallBody);
    }();
    AllocationObservation largeObservation;
    const auto largePlan = [&] {
        ScopedAllocationObservation scope(largeObservation);
        return checkpointStoragePlan(largeBody);
    }();
    REQUIRE(smallPlan && largePlan);
    CHECK(smallPlan->validationScratchBytes == largePlan->validationScratchBytes);
    CHECK(smallObservation.attempts.load() == largeObservation.attempts.load());
    CHECK(largePlan->screenCellBytes == 2 * 31 * 129 * sizeof(Ubiquitty::Cell));
    CHECK(largePlan->stateStringBytes == 1000);
}

TEST(checkpointPlanningEveryFixedScratchRefusalIsTypedAndRetryable)
{
    auto host = makeTerminal();
    const auto body = checkpointOf(*host);
    AllocationObservation measurement;
    const auto measured = [&] {
        ScopedAllocationObservation scope(measurement);
        return checkpointStoragePlan(body);
    }();
    REQUIRE(measured);
    REQUIRE(measurement.attempts.load() > 0);
    for (std::size_t ordinal = 1; ordinal <= measurement.attempts.load(); ++ordinal) {
        AllocationObservation observation { .refusedAttemptOrdinal = ordinal };
        const auto refused = [&] {
            ScopedAllocationObservation scope(observation);
            return checkpointStoragePlan(body);
        }();
        REQUIRE(!refused);
        CHECK(refused.error() == ApplyOutcome::AllocationRefused);
        CHECK(observation.refusals.load() == 1);
        CHECK(checkpointOf(*host) == body);
    }
    const auto retried = checkpointStoragePlan(body);
    REQUIRE(retried);
    CHECK(*retried == *measured);
}

TEST(checkpointPlanningAndPreparationAgreeOnEveryTruncationAndDigest)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "primary\x1b[?1049halternate");
    const auto body = checkpointOf(*host);
    for (std::size_t length = 0; length < body.size(); ++length) {
        const auto bytes = std::span(body).first(length);
        const auto plan = checkpointStoragePlan(bytes);
        const auto prepared = prepareCheckpoint(bytes);
        REQUIRE(!plan && !prepared);
        CHECK(plan.error() == prepared.error());
    }
    auto changed = body;
    changed.back() ^= 1;
    const auto plan = checkpointStoragePlan(changed);
    const auto prepared = prepareCheckpoint(changed);
    REQUIRE(!plan && !prepared);
    CHECK(plan.error() == ApplyOutcome::DigestMismatch);
    CHECK(plan.error() == prepared.error());
}

TEST(checkpointPlanningUsesTheSameNestedStateAndRecordValidation)
{
    auto host = makeTerminal();
    const auto body = checkpointOf(*host);
    WireReader reader(body);
    (void)reader.take(17);
    const auto modes = reader.bytes(reader.remaining());
    REQUIRE(!modes.empty());
    auto changed = body;
    changed[static_cast<std::size_t>(modes.data() - body.data())] = 2;
    auto plan = checkpointStoragePlan(changed);
    auto prepared = prepareCheckpoint(changed);
    REQUIRE(!plan && !prepared);
    CHECK(plan.error() == ApplyOutcome::Malformed && plan.error() == prepared.error());
    // The publisher is allowed to encode this privately constructed invalid
    // state; both receiver paths must reject its consistent wire digest.
    host->programStatus.records.push_back({ "job", {}, {}, {}, Ubiquitty::OSC::ProgramState::Working,
        Ubiquitty::OSC::ProgramBlockKind::None, 101 });
    changed = checkpointOf(*host);
    plan = checkpointStoragePlan(changed);
    prepared = prepareCheckpoint(changed);
    REQUIRE(!plan && !prepared);
    CHECK(plan.error() == ApplyOutcome::Malformed && plan.error() == prepared.error());
}

TEST(checkpointPlanningAndPreparationRejectMalformedRowsAndDocuments)
{
    auto host = makeTerminal();
    Ubiquitty::RichText::Block block;
    block.serial = 17;
    block.width = 5;
    block.height = 1;
    block.document.text = { 'o', 'k' };
    block.document.runs.push_back({ .length = 2 });
    host->richTextBlocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    const auto body = checkpointOf(*host);
    REQUIRE(checkpointStoragePlan(body));
    WireReader reader(body);
    (void)reader.take(17);
    for (std::size_t index = 0; index < stateBlockCount; ++index)
        (void)reader.bytes(reader.remaining());
    const auto pens = reader.count(std::numeric_limits<std::uint32_t>::max(), penWireBytes);
    (void)reader.take(pens * penWireBytes);
    REQUIRE(reader.count(0, 4) == 0 && reader.ok()); // no links in this fixture
    REQUIRE(reader.count(0, 2) == 0 && reader.ok()); // no clusters
    REQUIRE(reader.count(0, 3) == 0 && reader.ok()); // no proportional runs
    REQUIRE(reader.count(2 * Ubiquitty::RichText::maxBlocksPerScreen, 6) == 1);
    REQUIRE(reader.varint(std::numeric_limits<std::uint32_t>::max()) == 17);
    const auto documentBegin = reader.offset();
    WireReader textReader(std::span { body }.subspan(documentBegin));
    REQUIRE(textReader.count(Ubiquitty::RichText::maxCodePoints, 1) == 2);
    const auto firstCodePoint = documentBegin + textReader.offset();
    REQUIRE(body[firstCodePoint] == 'o');
    auto changed = body;
    changed[firstCodePoint] = 0; // RichText requires nonzero Unicode scalars.
    auto plan = checkpointStoragePlan(changed);
    auto prepared = prepareCheckpoint(changed);
    REQUIRE(!plan && !prepared);
    CHECK(plan.error() == ApplyOutcome::Malformed && plan.error() == prepared.error());
    REQUIRE(readDocumentView(reader));
    const auto firstRow = reader.bytes(reader.remaining());
    REQUIRE(reader.ok() && firstRow.size() >= 3);
    changed = body;
    changed[static_cast<std::size_t>(firstRow.data() - body.data()) + 1] = 2;
    plan = checkpointStoragePlan(changed);
    prepared = prepareCheckpoint(changed);
    REQUIRE(!plan && !prepared);
    CHECK(plan.error() == ApplyOutcome::Malformed && plan.error() == prepared.error());
}

TEST(checkpointPlanningAndMaterializationRefuseExtremePlacementRows)
{
    auto host = makeTerminal();
    Ubiquitty::RichText::Block block;
    block.serial = 17;
    block.width = 5;
    block.height = 1;
    block.document.text = { 'o', 'k' };
    block.document.runs.push_back({ .length = 2 });
    const std::array<RichTextDefinition, 1> definitions { RichTextDefinition { 17, block.document } };
    const std::array<RichTextDefinitionView, 1> views { RichTextDefinitionView { 17, { {}, 2, 1 } } };
    host->richTextBlocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    const auto body = checkpointOf(*host);
    WireReader reader(body);
    const auto header = reader.take(17);
    std::array<std::span<const std::uint8_t>, stateBlockCount> blocks;
    for (auto& bytes : blocks)
        bytes = reader.bytes(reader.remaining());
    REQUIRE(reader.ok());
    const auto suffix = reader.take(reader.remaining());
    const auto placementAt = [](std::int64_t row, std::uint8_t screen) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.varint(1);
        writer.u8(screen);
        writer.varint(17);
        writer.u16(0);
        writer.u32(0); // No application document revision.
        writer.svarint(row);
        writer.u16(0);
        writer.u16(5);
        writer.u16(1);
        writer.u8(std::to_underlying(Ubiquitty::RichText::Overflow::Wrap));
        writer.u8(std::to_underlying(Ubiquitty::RichText::Alignment::Left));
        writer.u8(std::to_underlying(Ubiquitty::RichText::LineMode::Natural));
        writer.u8(std::to_underlying(Ubiquitty::RichText::PointerMode::Cells));
        writer.u8(std::to_underlying(Ubiquitty::RichText::Lifetime::Cells));
        writer.u16(65535); // No caret.
        writer.u16(0); // Viewport top.
        for (int edge = 0; edge < 4; ++edge)
            writer.u8(0); // No padding.
        writer.u8(std::to_underlying(Ubiquitty::RichText::VerticalPlacement::Top));
        writer.varint(0); // No clip rectangles.
        return bytes;
    };
    for (const auto row : { std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max() }) {
        const auto placements = placementAt(row, 0);
        CHECK(!richTextPlacementStorage(placements, 7, 23, views));
        Ubiquitty::RichText::BlockStore materialized;
        CHECK(!readRichTextBlock(placements, 7, 23, definitions, {}, materialized));
        std::vector<std::uint8_t> changed;
        WireWriter writer(changed);
        writer.raw(header);
        // The rich-text block is last in the canonical checkpoint block order.
        for (std::size_t index = 0; index < blocks.size(); ++index)
            writer.bytes(index + 1 == blocks.size() ? std::span<const std::uint8_t>(placements) : blocks[index]);
        writer.raw(suffix);
        // Semantic rejection precedes digest comparison; retaining the original
        // digest here prevents a false DigestMismatch-only regression.
        const auto plan = checkpointStoragePlan(changed);
        const auto prepared = prepareCheckpoint(changed);
        REQUIRE(!plan && !prepared);
        CHECK(plan.error() == ApplyOutcome::Malformed && prepared.error() == ApplyOutcome::Malformed);
    }
    const auto deepest = -static_cast<std::int64_t>(Ubiquitty::maxRows) * 1024;
    for (const auto row : { std::int64_t { -1 }, deepest, std::int64_t { 6 } }) {
        const auto placements = placementAt(row, 0);
        REQUIRE(richTextPlacementStorage(placements, 7, 23, views));
        Ubiquitty::RichText::BlockStore materialized;
        REQUIRE(readRichTextBlock(placements, 7, 23, definitions, {}, materialized));
        REQUIRE(materialized.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
        CHECK(materialized.blocks(Ubiquitty::ScreenMode::Primary).front().row == row);
    }
    const auto alternateHistory = placementAt(-1, 1);
    CHECK(!richTextPlacementStorage(alternateHistory, 7, 23, views));
    Ubiquitty::RichText::BlockStore materialized;
    CHECK(!readRichTextBlock(alternateHistory, 7, 23, definitions, {}, materialized));
}

TEST(checkpointMetadataOwnerRefusalKeepsTerminalReplicaHistoryAndStatus)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "\x1b]8;id=new;https://example.test/new\x1b\\linked\x1b]8;;\x1b\\");
    host->udk = std::make_unique<Ubiquitty::VT::UserDefinedKeys>();
    host->udk->locked = true;
    host->udk->count = 1;
    host->udk->entries[0].keyCode = 11;
    host->udk->entries[0].length = 3;
    std::memcpy(host->udk->entries[0].value, "new", 3);
    setStatusRow(*host, 'H', 'T');
    const auto body = checkpointOf(*host);
    const auto plan = checkpointStoragePlan(body);
    REQUIRE(plan);
    CHECK(plan->linkTableBytes == sizeof(Ubiquitty::Attributes::Hyperlinks::LinkTable));
    CHECK(plan->userDefinedKeysBytes == sizeof(Ubiquitty::VT::UserDefinedKeys));
    auto client = makeTerminal(3, 11);
    UbiquittyTest::feed(*client, "\x1b]8;id=old;https://example.test/old\x1b\\old\x1b]8;;\x1b\\");
    client->udk = std::make_unique<Ubiquitty::VT::UserDefinedKeys>();
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    UbiquittyTest::feed(*client, "\r\none\r\ntwo\r\nthree\r\nfour\r\n");
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const auto* links = client->linkTable.get();
    const auto* keys = client->udk.get();
    const auto* status = client->statusRow.get();
    const auto history = Ubiquitty::Screen::scrollbackCount(client.get());
    REQUIRE(links && keys && status && history > 0);
    for (unsigned owner = 0; owner < 4; ++owner) {
        AllocationObservation observation;
        observation.rowAttributeArrayBytes = 7 * sizeof(RowAttributes);
        observation.refusedAttributeOrdinal = owner < 2 ? owner + 1 : 0;
        observation.refusesLinkTableOwner = owner == 2;
        observation.refusesUserDefinedKeysOwner = owner == 3;
        ApplyOutcome outcome;
        {
            ScopedAllocationObservation scope(observation);
            outcome = applyCheckpoint(*client, body, replica);
        }
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK(observation.refusals.load() == 1);
        if (owner < 2)
            CHECK(observation.attributeArrays.load() == owner + 1);
        else if (owner == 2)
            CHECK(observation.linkTableOwners.load() == 1);
        else
            CHECK(observation.userDefinedKeysOwners.load() == 1);
        CHECK(checkpointOf(*client) == before);
        CHECK(client->linkTable.get() == links);
        CHECK(client->udk.get() == keys);
        CHECK(client->statusRow.get() == status);
        CHECK(Ubiquitty::Screen::scrollbackCount(client.get()) == history);
        checkReplicaUnchanged(replica, beforeReplica);
    }
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    REQUIRE(client->udk);
    CHECK(client->udk->locked && client->udk->count == 1);
    CHECK(client->udk->entries[0].length == 3);
    CHECK(std::memcmp(client->udk->entries[0].value, "new", 3) == 0);
}

TEST(checkpointHyperlinkRingMovesAfterWireReleaseAndContinuesIDs)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    for (const bool exhaustsIDs : { false, true }) {
        auto host = makeTerminal();
        for (int index = 0; index < Ubiquitty::VT::linkTableCapacity + 3; ++index)
            REQUIRE(Links::intern(host.get(), {}, "https://example.test/" + std::to_string(index)) != 0);
        if (exhaustsIDs) {
            host->linkNextID = 0xFFFF;
            REQUIRE(Links::intern(host.get(), "last", "https://example.test/last") == 0xFFFF);
            REQUIRE(host->linkNextID == 0);
        }
        REQUIRE(host->linkHead != 0);
        auto body = checkpointOf(*host);
        AllocationObservation preparation;
        auto prepared = [&] {
            ScopedAllocationObservation scope(preparation);
            return prepareCheckpoint(body);
        }();
        REQUIRE(prepared);
        REQUIRE(preparation.linkTableOwners.load() == 1);
        const auto* staged = preparation.linkTableOwner.load();
        REQUIRE(staged);
        std::fill(body.begin(), body.end(), 0xA5);
        std::vector<std::uint8_t>().swap(body);
        auto client = makeTerminal();
        REQUIRE(Links::intern(client.get(), {}, "https://old.test/") != 0);
        ScreenReplica replica;
        AllocationObservation commit { .refusesLinkTableOwner = true };
        ApplyOutcome outcome;
        {
            ScopedAllocationObservation scope(commit);
            outcome = commitCheckpoint(*client, std::move(*prepared), replica);
        }
        REQUIRE(outcome == ApplyOutcome::Applied);
        CHECK(commit.linkTableOwners.load() == 0);
        CHECK(client->linkTable.get() == staged);
        CHECK(client->linkHead == 0 && client->linkCount == host->linkCount);
        CHECK(client->linkNextID == host->linkNextID);
        for (int index = 0; index < host->linkCount; ++index) {
            const auto oldSlot = static_cast<std::size_t>((host->linkHead + index) % Ubiquitty::VT::linkTableCapacity);
            const auto slot = static_cast<std::size_t>(index);
            CHECK(client->linkTable->ids[slot] == host->linkTable->ids[oldSlot]);
            CHECK(client->linkTable->hashes[slot] == host->linkTable->hashes[oldSlot]);
            CHECK(client->linkTable->keys[slot] == host->linkTable->keys[oldSlot]);
            CHECK(client->linkTable->uris[slot] == host->linkTable->uris[oldSlot]);
        }
        CHECK(Links::intern(client.get(), {}, "https://example.test/next") == Links::intern(host.get(), {}, "https://example.test/next"));
        Links::reset(*host);
        REQUIRE(applyCheckpoint(*client, checkpointOf(*host), replica) == ApplyOutcome::Applied);
        CHECK(!client->linkTable && client->linkCount == 0 && client->linkNextID == 1);
    }
}

TEST(checkpointLinkCommitGridRefusalKeepsThePreviousRingAndOwners)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    auto host = makeTerminal(31, 129);
    REQUIRE(Links::intern(host.get(), {}, "https://new.test/") != 0);
    const auto body = checkpointOf(*host);
    auto prepared = prepareCheckpoint(body);
    REQUIRE(prepared);
    auto client = makeTerminal(3, 11);
    REQUIRE(Links::intern(client.get(), {}, "https://old.test/") != 0);
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    UbiquittyTest::feed(*client, "one\r\ntwo\r\nthree\r\nfour\r\n");
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const auto* links = client->linkTable.get();
    const auto* status = client->statusRow.get();
    const auto history = Ubiquitty::Screen::scrollbackCount(client.get());
    REQUIRE(history > 0);
    AllocationObservation observation { .refusedAttemptOrdinal = 1 };
    ApplyOutcome outcome;
    {
        ScopedAllocationObservation scope(observation);
        outcome = commitCheckpoint(*client, std::move(*prepared), replica);
    }
    CHECK(outcome == ApplyOutcome::AllocationRefused);
    CHECK(observation.refusals.load() == 1);
    CHECK(client->linkTable.get() == links);
    CHECK(client->statusRow.get() == status);
    CHECK(checkpointOf(*client) == before);
    CHECK(Ubiquitty::Screen::scrollbackCount(client.get()) == history);
    checkReplicaUnchanged(replica, beforeReplica);
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
}

TEST(replicaStorageCopiesRetainImmutableArraysWithoutAllocation)
{
    auto builder = ReplicaStorageBuilder::create(3, { 5, 7 }, 8);
    REQUIRE(builder);
    builder->pens()[1].foregroundIndex = 3;
    builder->rowHashes()[0][2] = 123;
    builder->rowHashes()[1][6] = 456;
    auto storage = std::move(*builder).publish();
    const auto* pens = storage.pens().data();
    const auto* rows = storage.rowHashes()[1].data();
    ReplicaStorage copy;
    AllocationObservation observation { .refusedAttemptOrdinal = 1 };
    {
        ScopedAllocationObservation scope(observation);
        ReplicaStorage intermediate = storage;
        copy = intermediate;
        auto* alias = &copy;
        copy = *alias;
        storage = {};
    }
    CHECK(observation.attempts.load() == 0);
    CHECK(copy.pens().data() == pens && copy.rowHashes()[1].data() == rows);
    CHECK(copy.pens()[1].foregroundIndex == 3);
    CHECK(copy.rowHashes()[0][2] == 123 && copy.rowHashes()[1][6] == 456);
    auto moved = std::move(copy);
    CHECK(copy.pens().empty() && copy.rowHashes()[0].empty());
    CHECK(moved.pens().data() == pens);
    const auto bytes = ReplicaStorageBuilder::storageBytesForCapacity(3, { 5, 7 });
    REQUIRE(bytes);
    CHECK(moved.allocationBytes() == *bytes);
}

TEST(replicaStorageEveryFinalOwnerRefusesWithoutPublishing)
{
    for (std::size_t ordinal = 1; ordinal <= 4; ++ordinal) {
        AllocationObservation observation { .refusedAttemptOrdinal = ordinal };
        const auto builder = [&] {
            ScopedAllocationObservation scope(observation);
            return ReplicaStorageBuilder::create(3, { 5, 7 }, 8);
        }();
        REQUIRE(!builder);
        CHECK(builder.error() == ReplicaStorageError::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal);
        CHECK(observation.refusals.load() == 1);
    }
    REQUIRE(ReplicaStorageBuilder::create(3, { 5, 7 }, 8));
}

TEST(replicaStorageGrowthRefusalKeepsOwnerAndAliasedPen)
{
    auto builder = ReplicaStorageBuilder::create(1, { 2, 2 }, 2);
    REQUIRE(builder);
    builder->pens()[0].foregroundIndex = 9;
    const auto* before = builder->pens().data();
    AllocationObservation observation { .refusedAttemptOrdinal = 1 };
    const auto refused = [&] {
        ScopedAllocationObservation scope(observation);
        return builder->appendPen(builder->pens()[0]);
    }();
    REQUIRE(!refused);
    CHECK(refused.error() == ReplicaStorageError::AllocationRefused);
    CHECK(builder->pens().data() == before && builder->pens().size() == 1);
    CHECK(builder->pens()[0].foregroundIndex == 9);
    REQUIRE(builder->appendPen(builder->pens()[0]));
    CHECK(builder->pens().size() == 2 && builder->pens()[1].foregroundIndex == 9);
    CHECK(!builder->appendPen(builder->pens()[0]));
    const auto storage = std::move(*builder).publish();
    CHECK(storage.penCapacity() == 2);
    const auto bytes = ReplicaStorageBuilder::storageBytesForCapacity(2, { 2, 2 });
    REQUIRE(bytes);
    CHECK(storage.allocationBytes() == *bytes);
}

TEST(replicaStorageCapacityArithmeticRefusesBeforeAllocation)
{
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        const auto overflow = ReplicaStorageBuilder::create(std::numeric_limits<std::size_t>::max(), { 1, 1 }, std::numeric_limits<std::size_t>::max());
        REQUIRE(!overflow);
        CHECK(overflow.error() == ReplicaStorageError::OverCapacity);
        const auto beyondObjectLimit = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Ubiquitty::Pen) + 1;
        const auto oversized = ReplicaStorageBuilder::create(beyondObjectLimit, { 0, 0 }, beyondObjectLimit);
        REQUIRE(!oversized);
        CHECK(oversized.error() == ReplicaStorageError::OverCapacity);
        CHECK(!ReplicaStorageBuilder::create(1, { static_cast<std::size_t>(Ubiquitty::maxRows) + 1, 1 }, 1));
        CHECK(!ReplicaStorageBuilder::create(2, { 1, 1 }, 1));
    }
    CHECK(observation.attempts.load() == 0);
    auto empty = ReplicaStorageBuilder::create(0, { 0, 0 }, 0);
    REQUIRE(empty);
    CHECK(!empty->appendPen(Ubiquitty::Pen {}));
    const auto storage = std::move(*empty).publish();
    CHECK(storage.pens().empty() && storage.rowHashes()[0].empty());
}

TEST(replicaStorageRefusesCountsPastItsNamedBoundsBeforeAllocation)
{
    // One past each bound, and a wrapped (negative) row count, are refused
    // before any array exists; the bounds themselves are admitted arithmetic.
    constexpr auto maxPens = ReplicaStorageBuilder::maxPenCapacity;
    constexpr auto maxRowCount = ReplicaStorageBuilder::maxRowCount;
    constexpr auto wrapped = static_cast<std::size_t>(-1);
    static_assert(maxPens <= std::numeric_limits<std::uint32_t>::max());
    AllocationObservation observation;
    {
        ScopedAllocationObservation scope(observation);
        const auto pens = ReplicaStorageBuilder::create(maxPens + 1, { 1, 1 }, wrapped);
        REQUIRE(!pens);
        CHECK(pens.error() == ReplicaStorageError::OverCapacity);
        const auto rows = ReplicaStorageBuilder::create(0, { wrapped, 1 }, 0);
        REQUIRE(!rows);
        CHECK(rows.error() == ReplicaStorageError::OverCapacity);
        CHECK(!ReplicaStorageBuilder::create(0, { 1, maxRowCount + 1 }, 0));
        CHECK(!ReplicaStorageBuilder::storageBytesForCapacity(maxPens + 1, { 0, 0 }));
        CHECK(!ReplicaStorageBuilder::storageBytesForCapacity(0, { 0, wrapped }));
    }
    CHECK(observation.attempts.load() == 0);
    const auto bounds = ReplicaStorageBuilder::storageBytesForCapacity(maxPens, { maxRowCount, maxRowCount });
    REQUIRE(bounds);
    CHECK(*bounds == *ReplicaStorageBuilder::storageBytesForCapacity(0, { 0, 0 }) + maxPens * sizeof(Ubiquitty::Pen) + 2 * maxRowCount * sizeof(std::uint64_t));
}

TEST(checkpointReplicaOwnerRefusalKeepsTerminalHistoryAndSnapshot)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "new picture");
    const auto body = checkpointOf(*host);
    auto client = makeTerminal(3, 11);
    UbiquittyTest::feed(*client, "old picture");
    setStatusRow(*client, 'B', 'Z');
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    UbiquittyTest::feed(*client, "\r\none\r\ntwo\r\nthree\r\nfour\r\n");
    const auto before = checkpointOf(*client);
    const auto held = replica;
    const auto* pens = replica.pens().data();
    const auto* status = client->statusRow.get();
    const auto history = Ubiquitty::Screen::scrollbackCount(client.get());
    REQUIRE(history > 0);
    // The two screen arrays precede the backing header, pens and two hashes.
    for (std::size_t ordinal = 3; ordinal <= 6; ++ordinal) {
        AllocationObservation observation { .refusedAttemptOrdinal = ordinal };
        const auto outcome = [&] {
            ScopedAllocationObservation scope(observation);
            return applyCheckpoint(*client, body, replica);
        }();
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal && observation.refusals.load() == 1);
        CHECK(checkpointOf(*client) == before);
        CHECK(client->statusRow.get() == status);
        CHECK(Ubiquitty::Screen::scrollbackCount(client.get()) == history);
        CHECK(replica.pens().data() == pens);
        checkReplicaUnchanged(replica, held);
    }
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    CHECK(replica.pens().data() != held.pens().data());
    CHECK(held.pens().data() == pens);
}

TEST(updateReplicaOwnersAndPenGrowthRefuseBeforeTerminalMutation)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "initial");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal();
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto before = checkpointOf(*client);
    const auto held = replica;
    UbiquittyTest::feed(*host, "\x1b[31mchanged");
    body.clear();
    REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    for (std::size_t ordinal = 1; ordinal <= 5; ++ordinal) {
        AllocationObservation observation { .refusedAttemptOrdinal = ordinal };
        const auto outcome = [&] {
            ScopedAllocationObservation scope(observation);
            return applyRowUpdate(*client, body, replica);
        }();
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal && observation.refusals.load() == 1);
        CHECK(checkpointOf(*client) == before);
        CHECK(replica.pens().data() == held.pens().data());
        checkReplicaUnchanged(replica, held);
    }
    REQUIRE(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    CHECK(replica.pens().size() > held.pens().size());
    CHECK(replica.pens().data() != held.pens().data());
    CHECK(held.stateDigest != replica.stateDigest);
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(updateClusterOwnerRefusalPreservesHeldPoolAndContinuation)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "a\xcc\x81\xcc\x82");
    REQUIRE(host->clusterPool.size() == 1);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal();
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto before = checkpointOf(*client);
    const auto held = replica;
    const auto* entries = client->clusterPool.entries().data();
    const auto continuation = client->clusterPool.continuation();
    UbiquittyTest::feed(*host, "b\xcc\x81\xcc\x82");
    REQUIRE(host->clusterPool.size() == 2);
    body.clear();
    REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    // Final replica owners precede the new pool's entries, index and buckets.
    for (std::size_t ordinal = 5; ordinal <= 7; ++ordinal) {
        AllocationObservation observation { .refusedAttemptOrdinal = ordinal };
        const auto outcome = [&] {
            ScopedAllocationObservation scope(observation);
            return applyRowUpdate(*client, body, replica);
        }();
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK(observation.attempts.load() == ordinal && observation.refusals.load() == 1);
        CHECK(checkpointOf(*client) == before);
        CHECK(client->clusterPool.entries().data() == entries);
        CHECK(client->clusterPool.continuation().nextID == continuation.nextID);
        CHECK(client->clusterPool.continuation().oldestSlot == continuation.oldestSlot);
        checkReplicaUnchanged(replica, held);
    }
    REQUIRE(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    CHECK(client->clusterPool.size() == 2);
    CHECK(std::ranges::equal(client->clusterPool.sequence(2), host->clusterPool.sequence(2)));
}

TEST(manyClusterDefinitionsUseAmortizedPrivateGrowthAndMatchHost)
{
    constexpr std::size_t heldClusters = 1536;
    constexpr std::size_t newClusters = 1024;
    constexpr int columns = 80;
    auto host = makeTerminal(40, columns);
    const auto addClusters = [&](std::size_t first, std::size_t count) {
        for (std::size_t index = first; index < first + count; ++index) {
            const std::array<std::uint32_t, 3> codePoints { 'a', 0x0300 + static_cast<std::uint32_t>(index / 112), 0x0300 + static_cast<std::uint32_t>(index % 112) };
            const auto id = host->clusterPool.intern(codePoints);
            REQUIRE(id != 0);
            auto cell = Ubiquitty::defaultBlankCell;
            cell.codePoint = 'a';
            cell.combining[0] = codePoints[1];
            cell.setCombiningLength(1);
            cell.setClusterID(id);
            host->grid->setCell(Ubiquitty::Row(static_cast<int>(index / columns)), Ubiquitty::Column(static_cast<int>(index % columns)), cell);
        }
    };
    addClusters(0, heldClusters);
    REQUIRE(host->clusterPool.entries().size() == heldClusters);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(40, columns);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    // Isolate array growth from the update scratch Terminal's fixed owners.
    replica.scratch = std::make_shared<Ubiquitty::Terminal>();
    addClusters(heldClusters, newClusters);
    REQUIRE(host->clusterPool.entries().size() == heldClusters + newClusters);
    body.clear();
    REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    AllocationObservation observation;
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return applyRowUpdate(*client, body, replica);
    }();
    REQUIRE(outcome == ApplyOutcome::Applied);
    // Four replica owners, then the pool's three clone arrays and one
    // geometric grow of those arrays, not three allocations per definition.
    CHECK(observation.attempts.load() == 4 + 2 * 3);
    CHECK(client->clusterPool.capacity() == 2 * (heldClusters + 1));
    CHECK(client->clusterPool.entries().size() == heldClusters + newClusters);
    for (std::uint16_t id = 1; id <= heldClusters + newClusters; ++id)
        CHECK(std::ranges::equal(client->clusterPool.sequence(id), host->clusterPool.sequence(id)));
    for (std::size_t screen = 0; screen < 2; ++screen)
        CHECK(std::ranges::equal(replica.rowHashes()[screen], baseline.rowHashes[screen]));
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(checkpointClusterOwnerMovesAfterWireReleaseWithoutCommitAllocation)
{
    auto host = makeTerminal();
    UbiquittyTest::feed(*host, "a\xcc\x81\xcc\x82");
    REQUIRE(host->clusterPool.size() == 1);
    auto body = checkpointOf(*host);
    auto prepared = prepareCheckpoint(body);
    REQUIRE(prepared);
    std::fill(body.begin(), body.end(), 0xA5);
    std::vector<std::uint8_t>().swap(body);
    auto client = makeTerminal();
    ScreenReplica replica;
    AllocationObservation observation { .refusedAttemptOrdinal = 1 };
    const auto outcome = [&] {
        ScopedAllocationObservation scope(observation);
        return commitCheckpoint(*client, std::move(*prepared), replica);
    }();
    CHECK(outcome == ApplyOutcome::Applied);
    CHECK(observation.attempts.load() == 0);
    REQUIRE(client->clusterPool.size() == host->clusterPool.size());
    CHECK(std::ranges::equal(client->clusterPool.sequence(1), host->clusterPool.sequence(1)));
    CHECK(client->clusterPool.continuation().nextID == host->clusterPool.continuation().nextID);
    CHECK(client->clusterPool.allocationBytes() == *Ubiquitty::ClusterPool::storageBytesForCapacity(1));
}

UBIQUITTY_TEST_MAIN()
