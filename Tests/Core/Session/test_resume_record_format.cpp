// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/ResumeRecordFormat.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

using namespace Ubiquitty::Protocol::Sync;
using namespace Ubiquitty::Session;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

namespace {

constexpr std::size_t budget = 1 << 20;
constexpr ID128 paneID { 9, 10 };

ResumeRecord emptyRecord()
{
    return { .hostInstanceID = { 1, 2 }, .clientInstance = { 3, 4 }, .attachmentID = { 5, 6 }, .resumeSecret = { 7, 8 }, .certificatePin = {}, .panes = { { .paneID = paneID, .lastInputSeq = 37, .view = ViewState::Dormant, .checkpoint = {}, .checkpointDigest = {}, .replica = {} } }, .drafts = {} };
}

ResumeRecord picturedRecord()
{
    ResumeRecord record = emptyRecord();
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    UbiquittyTest::feed(terminal, "\x1b[31mred\x1b[0m picture");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ResumeRecordPane& pane = record.panes.front();
    CHECK(publisher.capture(terminal));
    CHECK(publisher.checkpoint(terminal, 17, pane.checkpoint, baseline));
    Ubiquitty::Terminal passive;
    CHECK(applyCheckpoint(passive, pane.checkpoint, pane.replica) == ApplyOutcome::Applied);
    pane.checkpointDigest = digestOf(pane.checkpoint);
    // Host dictionary numbering is independent of the saved picture's own
    // publisher, so the checkpoint cannot reconstruct these host values.
    pane.replica.dictionaryEpoch += 29;
    auto storage = ReplicaStorageBuilder::create(pane.replica.pens().size() + 1,
        { pane.replica.rowHashes()[0].size(), pane.replica.rowHashes()[1].size() }, pane.replica.maxPens + 1);
    CHECK(storage.has_value());
    if (!storage)
        return record;
    std::ranges::copy(pane.replica.pens(), storage->pens().begin());
    storage->pens().back() = Ubiquitty::Pen {};
    for (std::size_t screen = 0; screen < 2; ++screen)
        std::ranges::copy(pane.replica.rowHashes()[screen], storage->rowHashes()[screen].begin());
    storage->rowHashes()[1][0] ^= 0x40;
    pane.replica.storage = std::move(*storage).publish();
    pane.replica.maxPens += 1;
    pane.replica.historyEpoch = 81;
    pane.replica.historyHead = 99;
    return record;
}

std::vector<std::uint8_t> bytesOf(const EncodedResumeRecord& record)
{
    return { record.view().begin(), record.view().end() };
}

} // namespace

TEST(resumeFormatPreservesPictureAndOriginalHostReplicaSeparately)
{
    const ResumeRecord record = picturedRecord();
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded.has_value());
    const auto decoded = decodeResumeRecord(encoded->view(), budget);
    REQUIRE(decoded.has_value());
    CHECK(decoded->clientInstance == record.clientInstance);
    CHECK(decoded->attachmentID == record.attachmentID);
    CHECK(decoded->resumeSecret == record.resumeSecret);
    REQUIRE(decoded->paneCount == 1);
    const auto& pane = decoded->panes[0];
    CHECK(pane.paneID == paneID);
    CHECK(pane.lastInputSeq == 37);
    CHECK(pane.view == ViewState::Dormant);
    CHECK(std::ranges::equal(pane.checkpoint, record.panes[0].checkpoint));
    WireReader replica(pane.replica);
    CHECK(replica.u64() == 17);
    CHECK(replica.u32() == record.panes[0].replica.dictionaryEpoch);
    CHECK(replica.u64() == record.panes[0].replica.maxPens);
    const auto pens = replica.u32();
    REQUIRE(pens == record.panes[0].replica.pens().size());
    for (std::size_t index = 0; index < pens; ++index) {
        const auto pen = readPen(replica);
        CHECK(std::memcmp(&pen, &record.panes[0].replica.pens()[index], sizeof(pen)) == 0);
    }
    for (const auto rows : record.panes[0].replica.rowHashes()) {
        CHECK(replica.u32() == rows.size());
        for (const auto hash : rows)
            CHECK(replica.u64() == hash);
    }
    (void)replica.take(stateBlockCount * 8);
    CHECK(replica.u32() == 81);
    CHECK(replica.u64() == 99);
}

TEST(resumeFormatKeepsAllIntentKindsOnlyAsDraftBytes)
{
    auto record = emptyRecord();
    record.drafts = {
        { paneID, RawInput { { 'r' } } },
        { paneID, KeyInput { { { .key = 65, .modifiers = 1, .kind = KeyEventKind::Repeat, .text = "\xc3\xa9", .unshiftedCodePoint = 'a', .shiftedCodePoint = 'A', .baseLayoutCodePoint = 'a', .wasModified = true } } } },
        { paneID, PointerInput { { {} } } },
        { paneID, FocusInput { Ubiquitty::Modes::FocusState::Focused } },
        { paneID, PasteInput { { 'p' }, 12, PasteEnd::Aborted } },
    };
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded.has_value());
    const auto decoded = decodeResumeRecord(encoded->view(), budget);
    REQUIRE(decoded.has_value());
    CHECK(decoded->draftCount == 5);
    CHECK(!decoded->drafts.empty());
    // No callback, connection or input writer is part of this API.
    CHECK(decoded->drafts.data() >= encoded->view().data());
    CHECK(decoded->drafts.data() + decoded->drafts.size() <= encoded->view().data() + encoded->size);
}

TEST(resumeFormatOwnsAndBudgetsOptionalHistoryAlongsideThePicture)
{
    auto record = picturedRecord();
    record.panes[0].history = { 'h', 'i', 's', 't', 'o', 'r', 'y' };
    auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    CHECK(!encodeResumeRecord(record, encoded->size - 1));
    const auto view = decodeResumeRecord(encoded->view(), budget);
    REQUIRE(view && view->paneCount == 1);
    CHECK(std::ranges::equal(view->panes[0].history, record.panes[0].history));
    CHECK(!materializeResumeRecord(encoded->view(), budget, view->materializedBytes - 1));
    auto materialized = materializeResumeRecord(encoded->view(), budget, view->materializedBytes);
    REQUIRE(materialized);
    encoded->bytes.reset();
    CHECK(materialized->panes[0].history == record.panes[0].history);
    CHECK(materialized->panes[0].checkpoint == record.panes[0].checkpoint);
}

TEST(resumeFormatRetainsPartialAndCompleteEffectReceiptsWithoutAPicture)
{
    for (const EffectReceiptState receipt : { EffectReceiptState {}, EffectReceiptState { 4, 7 }, EffectReceiptState { 9, 9 } }) {
        auto record = emptyRecord();
        record.panes[0].effects = receipt;
        const auto encoded = encodeResumeRecord(record, budget);
        REQUIRE(encoded);
        const auto decoded = decodeResumeRecord(encoded->view(), budget);
        REQUIRE(decoded && decoded->paneCount == 1);
        CHECK_EQ(decoded->panes[0].effects.acknowledged, receipt.acknowledged);
        CHECK_EQ(decoded->panes[0].effects.consumed, receipt.consumed);
        const auto materialized = materializeResumeRecord(encoded->view(), budget, budget);
        REQUIRE(materialized);
        CHECK_EQ(materialized->panes[0].effects.acknowledged, receipt.acknowledged);
        CHECK_EQ(materialized->panes[0].effects.consumed, receipt.consumed);
        CHECK(materialized->panes[0].checkpoint.empty());
    }
}

TEST(resumeFormatRejectsAnEffectAcknowledgementBeyondConsumption)
{
    auto record = emptyRecord();
    record.panes[0].effects = { 8, 7 };
    CHECK(!encodeResumeRecord(record, budget));
    record.panes[0].effects = { 7, 7 };
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    auto changed = bytesOf(*encoded);
    constexpr std::size_t acknowledgementOffset = 4 + 2 + 2 + 4 * 16 + 32 + 4 + 16 + 8 + 1;
    changed[acknowledgementOffset] = 8;
    CHECK(!decodeResumeRecord(changed, budget));
    CHECK(!materializeResumeRecord(changed, budget, budget));
}

TEST(resumeFormatRejectsEveryTruncationTrailingBytesAndOtherVersions)
{
    const auto encoded = encodeResumeRecord(picturedRecord(), budget);
    REQUIRE(encoded.has_value());
    for (std::size_t length = 0; length < encoded->size; ++length)
        CHECK(!decodeResumeRecord(encoded->view().first(length), budget));
    auto changed = bytesOf(*encoded);
    changed.push_back(0);
    CHECK(!decodeResumeRecord(changed, budget));
    changed = bytesOf(*encoded);
    changed[6] ^= 1;
    const auto wrongWire = decodeResumeRecord(changed, budget);
    REQUIRE(!wrongWire);
    CHECK(wrongWire.error() == ResumeRecordError::VersionMismatch);
    changed = bytesOf(*encoded);
    changed[4] ^= 1;
    CHECK(!decodeResumeRecord(changed, budget));
}

TEST(resumeFormatRejectsCorruptPictureBeforeAnyTerminalExists)
{
    const auto encoded = encodeResumeRecord(picturedRecord(), budget);
    REQUIRE(encoded.has_value());
    const auto decoded = decodeResumeRecord(encoded->view(), budget);
    REQUIRE(decoded.has_value());
    auto changed = bytesOf(*encoded);
    const auto offset = static_cast<std::size_t>(decoded->panes[0].checkpoint.data() - encoded->view().data());
    changed[offset + 18] ^= 1;
    CHECK(!decodeResumeRecord(changed, budget));
    // A length promising data past the immutable owner is refused as well.
    std::fill_n(changed.begin() + static_cast<std::ptrdiff_t>(offset - 4), 4, 0xff);
    CHECK(!decodeResumeRecord(changed, budget));
}

TEST(resumeFormatMeasuresBeforeAllocationAndRefusesNullAllocation)
{
    const auto record = emptyRecord();
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded.has_value());
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
    const auto tooLarge = encodeResumeRecord(record, encoded->size - 1);
    REQUIRE(!tooLarge);
    CHECK(tooLarge.error() == ResumeRecordError::TooLarge);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    const auto noMemory = encodeResumeRecord(record, encoded->size);
    REQUIRE(!noMemory);
    CHECK(noMemory.error() == ResumeRecordError::AllocationFailed);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts + 1);
    const auto decoded = decodeResumeRecord(encoded->view(), encoded->size);
    CHECK(decoded.has_value());
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts + 1);
}

TEST(resumeFormatRejectsDuplicatePaneOrOrphanDraftAndEnforcesDraftBudget)
{
    auto record = emptyRecord();
    record.panes.push_back(record.panes.front());
    CHECK(!encodeResumeRecord(record, budget));
    record = emptyRecord();
    record.drafts.push_back({ { 99, 100 }, RawInput { { 1 } } });
    CHECK(!encodeResumeRecord(record, budget));
    record.drafts = { { paneID, RawInput { std::vector<std::uint8_t>(maxLostInputBytes, 'x') } } };
    const auto exact = encodeResumeRecord(record, budget);
    REQUIRE(exact.has_value());
    CHECK(decodeResumeRecord(exact->view(), budget).has_value());
    record.drafts.push_back({ paneID, RawInput { {} } });
    CHECK(!encodeResumeRecord(record, budget));
}

TEST(resumeMaterializationOwnsEveryReplicaAndDraftField)
{
    auto record = picturedRecord();
    record.drafts = {
        { paneID, RawInput { { 'r', 0, 's' } } },
        { paneID, KeyInput { { { .key = 65, .modifiers = 1, .kind = KeyEventKind::Repeat, .text = "\xc3\xa9", .unshiftedCodePoint = 'a', .shiftedCodePoint = 'A', .baseLayoutCodePoint = 'a', .wasModified = true } } } },
        { paneID, PointerInput { { { .kind = PointerEventKind::Release, .button = 2, .modifiers = 3, .row = 4, .column = 5, .pixelX = 6, .pixelY = 7 } } } },
        { paneID, FocusInput { Ubiquitty::Modes::FocusState::Focused } },
        { paneID, PasteInput { { 'p', 0, 't' }, 12, PasteEnd::Aborted } },
    };
    auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    const auto owned = materializeResumeRecord(encoded->view(), budget, budget);
    REQUIRE(owned);
    std::fill_n(encoded->bytes.get(), encoded->size, 0);
    encoded->bytes.reset();
    const auto replay = encodeResumeRecord(*owned, budget);
    const auto original = encodeResumeRecord(record, budget);
    REQUIRE(replay && original);
    CHECK(std::ranges::equal(replay->view(), original->view()));
    REQUIRE(owned->panes.size() == 1);
    CHECK(!owned->panes[0].replica.scratch);
    CHECK(!owned->panes[0].replica.echoContext);
    // Every discriminant and event field survives, rather than converting
    // keys, pointers or focus into raw bytes that could be replayed.
    REQUIRE(owned->drafts.size() == record.drafts.size());
    for (std::size_t index = 0; index < record.drafts.size(); ++index)
        CHECK(owned->drafts[index].intent.index() == record.drafts[index].intent.index());
}

TEST(resumeMaterializationRefusesDecodedExpansionBeforePublishingState)
{
    const auto record = picturedRecord();
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    const auto& pane = record.panes[0];
    const auto storageBytes = ReplicaStorageBuilder::storageBytesForCapacity(pane.replica.pens().size(),
        { pane.replica.rowHashes()[0].size(), pane.replica.rowHashes()[1].size() });
    REQUIRE(storageBytes);
    const auto exact = sizeof(ResumeRecord) + sizeof(ResumeRecordPane) + pane.checkpoint.size() + *storageBytes;
    const auto refused = materializeResumeRecord(encoded->view(), budget, exact - 1);
    REQUIRE(!refused);
    CHECK(refused.error() == ResumeRecordError::TooLarge);
    CHECK(materializeResumeRecord(encoded->view(), budget, exact));
    for (std::size_t length = 0; length < encoded->size; ++length)
        CHECK(!materializeResumeRecord(encoded->view().first(length), budget, budget));
}

TEST(resumeReplicaFinalOwnerRefusalReturnsAllocationFailedAndKeepsSource)
{
    const auto record = picturedRecord();
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    const auto headerBytes = ReplicaStorageBuilder::storageBytesForCapacity(0, { 0, 0 });
    REQUIRE(headerBytes);
    const auto& replica = record.panes[0].replica;
    const auto* pens = replica.pens().data();
    for (const auto bytes : { *headerBytes, replica.pens().size_bytes(), replica.rowHashes()[0].size_bytes() }) {
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        const auto refused = [&] {
            UbiquittyTest::ScopedNothrowAllocationRefusal scope(bytes);
            return materializeResumeRecord(encoded->view(), budget, budget);
        }();
        REQUIRE(!refused);
        CHECK(refused.error() == ResumeRecordError::AllocationFailed);
        CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == refusals + 1);
        CHECK(replica.pens().data() == pens);
    }
    auto owned = materializeResumeRecord(encoded->view(), budget, budget);
    REQUIRE(owned);
    CHECK(owned->panes[0].replica.storage == replica.storage);
    CHECK(owned->panes[0].replica.pens().data() != pens);
}

TEST(resumePenCapacityUsesTheWireBoundAndThePlatformsStorageWidth)
{
    constexpr auto maxWirePens = std::uint64_t { std::numeric_limits<std::uint32_t>::max() } + maxPensPerEpoch;
    constexpr auto maxLocalPens = std::min<std::uint64_t>(maxWirePens, std::numeric_limits<std::size_t>::max());
    auto record = picturedRecord();
    record.panes[0].replica.maxPens = static_cast<std::size_t>(maxLocalPens);
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    const auto view = decodeResumeRecord(encoded->view(), budget);
    REQUIRE(view);
    const auto owned = materializeResumeRecord(encoded->view(), budget, budget);
    REQUIRE(owned);
    CHECK(owned->panes[0].replica.maxPens == maxLocalPens);

    // The fixed-width wire value can exceed either machine's size_t or the
    // protocol ceiling without allocating a corresponding dictionary.
    const auto capacityOffset = static_cast<std::size_t>(view->panes[0].replica.data() - encoded->view().data()) + 12;
    for (const auto invalid : { std::uint64_t { 0 }, maxWirePens + 1, std::numeric_limits<std::uint64_t>::max() }) {
        auto changed = bytesOf(*encoded);
        for (std::size_t byte = 0; byte < 8; ++byte)
            changed[capacityOffset + byte] = static_cast<std::uint8_t>(invalid >> (byte * 8));
        CHECK(!decodeResumeRecord(changed, budget));
    }
    record.panes[0].replica.maxPens = 0;
    CHECK(!encodeResumeRecord(record, budget));
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        record.panes[0].replica.maxPens = static_cast<std::size_t>(maxWirePens + 1);
        CHECK(!encodeResumeRecord(record, budget));
    }
}

UBIQUITTY_TEST_MAIN()

namespace {

/// A frontend's slot: bytes it keeps, and whether it accepts a write.
class TestSlot final : public ResumeRecordSlot {
public:
    std::optional<std::vector<std::uint8_t>> bytes;
    bool acceptsWrites = true;
    bool acceptsErase = true;

    bool write(std::span<const std::uint8_t> value) override
    {
        if (!acceptsWrites)
            return false;
        bytes.emplace(value.begin(), value.end());
        return true;
    }
    std::optional<std::vector<std::uint8_t>> read(std::size_t maxBytes) override
    {
        return bytes && bytes->size() <= maxBytes ? bytes : std::nullopt;
    }
    bool erase() override
    {
        if (!acceptsErase)
            return false;
        bytes.reset();
        return true;
    }
};

} // namespace

// The slot holds Core's encoding and nothing else: what is read back is the
// record written, a slot that refuses or a record over the budget commits
// nothing, bytes that are not a record read as none, and a purge is only as
// good as the slot's erase.
TEST(anEncodedStoreKeepsTheRecordInItsSlotAndRefusesWhatItCannotKeep)
{
    TestSlot slot;
    const ResumeRecord record = picturedRecord();
    const auto encoded = encodeResumeRecord(record, budget);
    REQUIRE(encoded);
    EncodedResumeRecordStore store(slot, budget);
    CHECK(store.historyPersistence() == HistoryPersistence::Disabled);
    REQUIRE(store.write(record) == ResumeWriteOutcome::Committed);
    REQUIRE(slot.bytes.has_value());
    CHECK(std::ranges::equal(*slot.bytes, encoded->view()));
    const auto read = store.read();
    REQUIRE(read.has_value());
    const auto again = encodeResumeRecord(*read, budget);
    REQUIRE(again);
    CHECK(std::ranges::equal(again->view(), encoded->view()));

    EncodedResumeRecordStore small(slot, encoded->size - 1);
    CHECK(small.write(record) == ResumeWriteOutcome::Unchanged);
    CHECK(!small.read().has_value());
    slot.acceptsWrites = false;
    CHECK(store.write(emptyRecord()) == ResumeWriteOutcome::Unchanged);
    CHECK(std::ranges::equal(*slot.bytes, encoded->view()));

    slot.bytes->back() ^= 1;
    CHECK(!store.read().has_value());
    slot.acceptsErase = false;
    CHECK(store.purge() == ResumePurgeOutcome::Refused);
    slot.acceptsErase = true;
    CHECK(store.purge() == ResumePurgeOutcome::Purged);
    CHECK(!slot.bytes.has_value());
    CHECK(!store.read().has_value());
}
