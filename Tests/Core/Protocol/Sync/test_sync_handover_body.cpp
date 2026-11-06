// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Image/TerminalImageInternal.h"
#include "Core/LineStore.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Engine.h"
#include "Core/VT/Parser.h"
#include "Core/VT/SoftFont.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

TEST(retainedEffectContinuationKeepsImmutableOwnersAndSentBoundaries)
{
    const ID128 pane { 11, 12 }, client { 21, 22 }, reader { 31, 32 };
    EffectLog source(pane);
    REQUIRE(source.addReader(reader, client, 0));
    const auto body = encodeEffectBody(BellEffect {});
    REQUIRE(body);
    REQUIRE(source.append({ .generation = 7, .primary = client, .kind = EffectKind::Bell, .payload = body->view() }));
    REQUIRE(source.noteSent(reader, 1));
    REQUIRE(source.acknowledge(reader, 1));
    source.trim(1);
    const std::array<std::uint8_t, 4> immutable { 1, 2, 3, 4 };
    REQUIRE(source.append({ .generation = 8, .primary = client, .kind = EffectKind::Sound, .payload = {}, .data = immutable }));
    REQUIRE(source.noteSent(reader, 2));
    REQUIRE(source.noteState(reader, AttachmentState::Lost, 900));
    const auto snapshot = source.continuation();
    EffectLog restored(pane);
    REQUIRE(restored.restoreContinuation(snapshot));
    CHECK_EQ(restored.retiredThrough(), 1u);
    CHECK_EQ(restored.newestSequence(), 2u);
    REQUIRE(restored.resumeReader(reader, client, 1));
    CHECK(!restored.acknowledge(reader, 3));
    auto batch = restored.readAfter(1, EffectReadMode::Consecutive);
    REQUIRE(batch && batch->size == 1);
    CHECK(batch->records[0].primary == client);
    CHECK(batch->records[0].data.data() != immutable.data());
    CHECK(batch->records[0].data.data() != snapshot.records[0].data.data());
    CHECK(std::ranges::equal(batch->records[0].data, immutable));
    REQUIRE(restored.acknowledge(reader, 2));
    restored.trim(901);
    CHECK_EQ(restored.size(), 0u);
    CHECK_EQ(restored.append({ .generation = 8, .kind = EffectKind::Bell, .payload = body->view() }).value(), 3u);
}

TEST(retainedEffectContinuationRefusesBeforeMutationAndDropsCarrierLeases)
{
    const ID128 pane { 11, 12 };
    EffectLog source(pane);
    auto lease = std::make_shared<EffectReaderLifetime>();
    REQUIRE(source.addTransientReader({ 21, 22 }, { 31, 32 }, 0, lease));
    const std::array<std::uint8_t, 7> data { 1, 2, 3, 4, 5, 6, 7 };
    REQUIRE(source.append({ .generation = 8, .kind = EffectKind::Sound, .payload = {}, .data = data }));
    auto snapshot = source.continuation();
    CHECK_EQ(snapshot.readerCount, 0u);
    EffectLog restored(pane);
    snapshot.records[0].sequence = 2;
    CHECK(!restored.restoreContinuation(snapshot));
    CHECK_EQ(restored.newestSequence(), 0u);
    snapshot.records[0].sequence = 1;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(data.size());
        CHECK(restored.restoreContinuation(snapshot).error() == EffectRefusal::NoMemory);
    }
    CHECK_EQ(restored.newestSequence(), 0u);
    CHECK_EQ(restored.retainedBytes(), 0u);
    REQUIRE(restored.restoreContinuation(snapshot));
    CHECK_EQ(restored.readerCount(), 0u);
    CHECK(!restored.restoreContinuation(snapshot));
}

TEST(retainedEffectContinuationPreservesLiveReadersLatentLostClock)
{
    const ID128 pane { 11, 12 }, reader { 21, 22 }, client { 31, 32 };
    const EffectPolicy policy { .lostGraceMicroseconds = 100 };
    EffectLog source(pane, policy);
    REQUIRE(source.addReader(reader, client, 0));
    const auto body = encodeEffectBody(BellEffect {});
    REQUIRE(body);
    REQUIRE(source.append({ .generation = 7, .kind = EffectKind::Bell, .payload = body->view() }));
    REQUIRE(source.noteSent(reader, 1));
    REQUIRE(source.noteState(reader, AttachmentState::Lost, 10));
    REQUIRE(source.noteState(reader, AttachmentState::Live, 20));
    const auto snapshot = source.continuation();
    CHECK_EQ(snapshot.readers[0].lostSinceMicroseconds, 10u);
    EffectLog restored(pane, policy);
    REQUIRE(restored.restoreContinuation(snapshot));
    CHECK_EQ(restored.continuation().readers[0].lostSinceMicroseconds, 10u);
    REQUIRE(restored.acknowledge(reader, 1));
    REQUIRE(restored.noteState(reader, AttachmentState::Lost, 300));
    CHECK_EQ(restored.continuation().readers[0].lostSinceMicroseconds, 300u);
    restored.trim(399);
    CHECK_EQ(restored.readerCount(), 1u);
    restored.trim(400);
    CHECK_EQ(restored.readerCount(), 0u);
}

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
constexpr HandoverHeaderLimits limits { 64, 4096 };
// These bytes pin the header independently of the writer. The body carries
// both terminal font sizes.
constexpr std::array<std::uint8_t, 41> pinnedHeader {
    static_cast<std::uint8_t>(handoverBodyVersion), 0,
    2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
    1,
    4, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0,
    2, 0, 0, 1, 0, 0
};
std::vector<std::uint8_t> encoded(const HandoverHeader& header)
{
    std::vector<std::uint8_t> bytes;
    encodeHandoverHeader(bytes, header);
    return bytes;
}
}

TEST(aNewerBodyIsRefusedBeforeReadingItsUnknownGrammar)
{
    const std::array<std::uint8_t, 2> bytes { static_cast<std::uint8_t>(handoverBodyVersion + 1), 0 };
    const auto result = decodeHandoverHeader(bytes, limits);
    REQUIRE(!result);
    CHECK(result.error() == HandoverBodyError::NewerVersion);
    CHECK(!decodeHandoverHeader(std::span(bytes).first(1), limits));
    const std::array<std::uint8_t, 2> zero { 0, 0 };
    CHECK(decodeHandoverHeader(zero, limits).error() == HandoverBodyError::Malformed);
}

TEST(thePinnedHeaderRetainsItsMeaning)
{
    const HandoverHeader expected { { 2, 3 }, { { 4, 5 } }, 2, 256 };
    const auto decoded = decodeHandoverHeader(pinnedHeader, limits);
    REQUIRE(decoded);
    CHECK(*decoded == expected);
    CHECK(std::ranges::equal(encoded(expected), pinnedHeader));
    std::vector<std::uint8_t> appended { 99 };
    encodeHandoverHeader(appended, expected);
    CHECK(appended.front() == 99);
    CHECK(std::span(appended).subspan(1).size() == pinnedHeader.size());
}

// A header below the supported release floor is refused before its fields.
TEST(anEarlierHeaderVersionIsRefused)
{
    auto earlier = std::vector<std::uint8_t>(pinnedHeader.begin(), pinnedHeader.end());
    for (std::uint16_t version = 1; version < oldestHandoverBodyVersion; ++version) {
        earlier[0] = static_cast<std::uint8_t>(version);
        const auto refused = decodeHandoverHeader(earlier, limits);
        REQUIRE(!refused);
        CHECK(refused.error() == HandoverBodyError::Malformed);
    }
}

TEST(aHeaderRejectsEveryTruncationAndAnySuffix)
{
    for (std::size_t length = 0; length < pinnedHeader.size(); ++length)
        CHECK(!decodeHandoverHeader(std::span(pinnedHeader).first(length), limits));
    auto extra = std::vector<std::uint8_t>(pinnedHeader.begin(), pinnedHeader.end());
    extra.push_back(0);
    CHECK(!decodeHandoverHeader(extra, limits));
}

TEST(headerCountsAreBoundedByTheReceiver)
{
    HandoverHeader header { { 1, 2 }, {}, 64, 4096 };
    for (std::uint64_t i = 0; i < maxInheritedInstances; ++i)
        header.inherited.push_back({ i + 3, 4 });
    CHECK(decodeHandoverHeader(encoded(header), limits).has_value());
    header.inherited.push_back({ 20, 30 });
    CHECK(decodeHandoverHeader(encoded(header), limits).error() == HandoverBodyError::OverCapacity);
    header.inherited.pop_back();
    header.paneCount = 65;
    CHECK(decodeHandoverHeader(encoded(header), limits).error() == HandoverBodyError::OverCapacity);
    header.paneCount = 64;
    header.tableBytes = 4097;
    CHECK(decodeHandoverHeader(encoded(header), limits).error() == HandoverBodyError::OverCapacity);
    CHECK(decodeHandoverHeader(encoded({ { 1, 2 }, {}, 0, 0 }), {}).has_value());
    CHECK(!decodeHandoverHeader(encoded({ { 1, 2 }, {}, 1, 0 }), {}));
    CHECK(!decodeHandoverHeader(encoded({ { 1, 2 }, {}, 0, 1 }), {}));
}

TEST(aHeaderCannotInventEmptyOrRepeatedIssuers)
{
    for (const auto& header : std::array<HandoverHeader, 4> {
             HandoverHeader { {}, {}, 0, 0 },
             { { 1, 2 }, { {} }, 0, 0 },
             { { 1, 2 }, { { 1, 2 } }, 0, 0 },
             { { 1, 2 }, { { 3, 4 }, { 3, 4 } }, 0, 0 } }) {
        const auto decoded = decodeHandoverHeader(encoded(header), limits);
        REQUIRE(!decoded);
        CHECK(decoded.error() == HandoverBodyError::Malformed);
    }
}

TEST(anUnboundedOrNoncanonicalAncestryCountIsRefused)
{
    auto bytes = std::vector<std::uint8_t>(pinnedHeader.begin(), pinnedHeader.begin() + 18);
    bytes.push_back(9);
    CHECK(decodeHandoverHeader(bytes, limits).error() == HandoverBodyError::OverCapacity);
    bytes.back() = 0x81;
    bytes.push_back(0);
    CHECK(!decodeHandoverHeader(bytes, limits));
    bytes.back() = 0x80;
    bytes.insert(bytes.end(), 20, 0x80);
    CHECK(!decodeHandoverHeader(bytes, limits));
}

namespace {

constexpr HandoverImageLimits imageLimits { 8192, Ubiquitty::Image::terminalImageBytesBudgetDefault };

void imageFixture(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::resize(terminal, 6, 40);
    UbiquittyTest::feed(terminal, "\033_Ga=T,f=32,s=1,v=1,i=7,p=9,c=2,r=3,C=1,q=2;/wAA/w==\033\\"
                                  "\033_Ga=f,f=32,s=1,v=1,i=7,z=60,q=2;AP8A/w==\033\\"
                                  "\033_Ga=a,i=7,c=3,r=2,q=2;\033\\"
                                  "\033[?1049h\033_Ga=p,i=7,p=10,c=1,r=1,C=1,q=2;\033\\\033[?1049l");
    terminal.images.kittyNextImageID = 0x80000007;
    terminal.images.kittyImageUseSeq += 11;
    terminal.images.kittyImageContentSeq += 13;
}

void refreshImageDigest(std::vector<std::uint8_t>& bytes)
{
    const auto digest = digestOf(std::span(bytes).first(bytes.size() - 16));
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[bytes.size() - 16 + index] = static_cast<std::uint8_t>(digest.low >> (index * 8));
        bytes[bytes.size() - 8 + index] = static_cast<std::uint8_t>(digest.high >> (index * 8));
    }
}

} // namespace

TEST(imageHandoverPreservesAnimationPlacementsAndAllocatorState)
{
    Ubiquitty::Terminal source;
    imageFixture(source);
    REQUIRE(source.images.stored[0].frameCount == 2);
    REQUIRE(source.images.stored[0].currentFrame == 1);
    REQUIRE(source.images.livePlacementCount == 2);
    source.images.stored[0].frameClockMilliseconds = 17;
    const auto encodedImages = encodeHandoverImages(source, imageLimits);
    REQUIRE(encodedImages);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverImages(encodedImages->view(), successor, imageLimits));
    const auto roundTrip = encodeHandoverImages(successor, imageLimits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(roundTrip->view(), encodedImages->view()));
    const auto& image = successor.images.stored[0];
    CHECK(image.pixels != source.images.stored[0].pixels);
    CHECK(image.frames[0].pixels == image.pixels);
    CHECK(image.frames[1].pixels != source.images.stored[0].frames[1].pixels);
    CHECK(image.frames[1].pixels != image.pixels);
    CHECK_EQ(image.frameClockMilliseconds, 17u);
    CHECK_EQ(Ubiquitty::Image::allocateImageID(successor), Ubiquitty::Image::allocateImageID(source));
    Ubiquitty::Image::advanceFrames(&source, 100);
    Ubiquitty::Image::advanceFrames(&successor, 100);
    const auto sourceAfter = encodeHandoverImages(source, imageLimits);
    const auto successorAfter = encodeHandoverImages(successor, imageLimits);
    REQUIRE(sourceAfter && successorAfter);
    CHECK(std::ranges::equal(sourceAfter->view(), successorAfter->view()));
}

TEST(imageHandoverRefusesTruncationCorruptionAndReceiverLimitsWithoutMutation)
{
    Ubiquitty::Terminal source;
    imageFixture(source);
    const auto encodedImages = encodeHandoverImages(source, imageLimits);
    REQUIRE(encodedImages);
    Ubiquitty::Terminal destination;
    imageFixture(destination);
    auto* const retainedPixels = destination.images.stored[0].pixels;
    for (std::size_t length = 0; length < encodedImages->size; ++length)
        CHECK(!restoreHandoverImages(encodedImages->view().first(length), destination, imageLimits));
    auto changed = std::vector<std::uint8_t>(encodedImages->view().begin(), encodedImages->view().end());
    changed[30] ^= 1;
    CHECK(restoreHandoverImages(changed, destination, imageLimits).error() == HandoverBodyError::DigestMismatch);
    changed.assign(encodedImages->view().begin(), encodedImages->view().end());
    changed.push_back(0);
    CHECK(!restoreHandoverImages(changed, destination, imageLimits));
    CHECK(restoreHandoverImages(encodedImages->view(), destination, { encodedImages->size - 1, imageLimits.maxPixelBytes }).error() == HandoverBodyError::OverCapacity);
    CHECK(restoreHandoverImages(encodedImages->view(), destination, { imageLimits.maxSectionBytes, imageLimits.maxPixelBytes - 1 }).error() == HandoverBodyError::OverCapacity);
    CHECK(destination.images.stored[0].pixels == retainedPixels);
    const auto untouched = encodeHandoverImages(destination, imageLimits);
    REQUIRE(untouched);
    CHECK(std::ranges::equal(untouched->view(), encodedImages->view()));
}

TEST(imageHandoverChecksCanonicalFlagsAndWholePixelBudget)
{
    Ubiquitty::Terminal source;
    imageFixture(source);
    source.images.kittyImageBytesBudget = 8;
    const auto encodedImages = encodeHandoverImages(source, { 8192, 8 });
    REQUIRE(encodedImages);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverImages(encodedImages->view(), successor, { 8192, 8 }));
    CHECK(restoreHandoverImages(encodedImages->view(), successor, { 8192, 7 }).error() == HandoverBodyError::OverCapacity);
    auto changed = std::vector<std::uint8_t>(encodedImages->view().begin(), encodedImages->view().end());
    // Version, allocator, placement count, budget, and the two serials precede slots.
    changed[34] = 2;
    refreshImageDigest(changed);
    CHECK(restoreHandoverImages(changed, successor, { 8192, 8 }).error() == HandoverBodyError::Malformed);
    source.images.kittyImageBytesBudget = 7;
    CHECK(encodeHandoverImages(source, imageLimits).error() == HandoverBodyError::OverCapacity);
}

TEST(imageHandoverRefusesOpenTransfersAndFallibleOutputAllocation)
{
    Ubiquitty::Terminal source;
    imageFixture(source);
    auto* const pixels = source.images.stored[0].pixels;
    source.images.kittyChunkActive = true;
    CHECK(encodeHandoverImages(source, imageLimits).error() == HandoverBodyError::NotAtGround);
    source.images.kittyChunkActive = false;
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        CHECK(encodeHandoverImages(source, imageLimits).error() == HandoverBodyError::AllocationRefused);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 1);
    CHECK(source.images.stored[0].pixels == pixels);
    CHECK(encodeHandoverImages(source, imageLimits).has_value());
}

namespace {

constexpr HandoverHistoryLimits historyLimits { 4 * 1024 * 1024, 20000, 1024, 100000 };

void historyFixture(Ubiquitty::Terminal& terminal, int rows)
{
    terminal.lineStore = std::make_unique<Ubiquitty::LineStore>();
    terminal.lineStore->init(512);
    terminal.lineStore->restartNumbering(700, Ubiquitty::LineStore::LineContinuity::ContinuesLine);
    terminal.historyEpoch = 9;
    terminal.historyEpochBase = 680;
    terminal.scrollbackCapacity = terminal.lineStore->capacity();
    std::array<Ubiquitty::Cell, 80> cells;
    cells.fill(Ubiquitty::defaultBlankCell);
    for (int index = 0; index < rows; ++index) {
        for (std::size_t column = 0; column < cells.size(); ++column) {
            auto& cell = cells[column];
            cell.codePoint = static_cast<std::uint32_t>('A' + (column + index) % 26);
            cell.pen.foregroundRed = static_cast<std::uint8_t>(index);
            cell.setLinkID(400);
            cell.setClusterID(123);
            cell.combining[0] = 0x301;
            cell.setCombiningLength(1);
        }
        terminal.lineStore->append(cells.data(), static_cast<int>(cells.size()), static_cast<std::uint8_t>(index % 4), static_cast<std::uint8_t>(index % 8), index % 3 != 2, true);
    }
    terminal.scrollbackCount = terminal.lineStore->count();
}

void writeLittle(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

} // namespace

TEST(historyHandoverPreservesFoldedRowsReferencesAndNumberingContinuation)
{
    Ubiquitty::Terminal source;
    historyFixture(source, 600);
    (void)source.lineStore->foldColdBlocks(20);
    REQUIRE(source.lineStore->foldedBlockCount() > 0);
    const auto bytes = encodeHandoverHistory(source, historyLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverHistory(bytes->view(), successor, historyLimits));
    const auto reproduced = encodeHandoverHistory(successor, historyLimits);
    REQUIRE(reproduced);
    CHECK(std::ranges::equal(bytes->view(), reproduced->view()));
    CHECK_EQ(successor.lineStore->totalAppended(), 600LL);
    CHECK_EQ(successor.historyEpoch, 9u);
    CHECK_EQ(successor.historyEpochBase, 680u);
    REQUIRE(successor.lineStore->at(0)->length == 80);
    CHECK_EQ(successor.lineStore->at(0)->cells[0].linkID(), 400);
    CHECK_EQ(successor.lineStore->at(0)->cells[0].clusterID(), 123);
    CHECK(successor.lineStore->at(0)->cells != source.lineStore->at(0)->cells);
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'Z';
    for (auto* terminal : { &source, &successor }) {
        terminal->lineStore->dropNewest(2);
        terminal->lineStore->append(&cell, 1, 0, 4, false, false);
        terminal->scrollbackCount = terminal->lineStore->count();
    }
    const auto afterSource = encodeHandoverHistory(source, historyLimits);
    const auto afterSuccessor = encodeHandoverHistory(successor, historyLimits);
    REQUIRE(afterSource && afterSuccessor);
    CHECK(std::ranges::equal(afterSource->view(), afterSuccessor->view()));
}

TEST(historyHandoverRejectsTruncationAndFalseCountsBeforeReplacingTheStore)
{
    Ubiquitty::Terminal source;
    historyFixture(source, 3);
    const auto bytes = encodeHandoverHistory(source, historyLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal destination;
    historyFixture(destination, 2);
    auto* const owner = destination.lineStore.get();
    const auto before = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(before);
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverHistory(bytes->view().first(length), destination, historyLimits));
    auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
    // The declared cell count starts after version, epoch/base/policy, store flag, capacity and rows.
    writeLittle(changed, 27, 239, 8);
    refreshImageDigest(changed);
    CHECK(restoreHandoverHistory(changed, destination, historyLimits).error() == HandoverBodyError::Malformed);
    changed.assign(bytes->view().begin(), bytes->view().end());
    // A row must not claim more columns than the model can represent.
    writeLittle(changed, 68, Ubiquitty::maxColumns + 1, 4);
    refreshImageDigest(changed);
    CHECK(restoreHandoverHistory(changed, destination, historyLimits).error() == HandoverBodyError::Malformed);
    CHECK(destination.lineStore.get() == owner);
    const auto after = encodeHandoverHistory(destination, historyLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
}

TEST(historyHandoverEnforcesIndependentDescriptorRowCellAndByteBounds)
{
    Ubiquitty::Terminal source;
    historyFixture(source, 3);
    const auto bytes = encodeHandoverHistory(source, historyLimits);
    REQUIRE(bytes);
    const HandoverHistoryLimits exact { bytes->size, static_cast<std::uint32_t>(source.scrollbackLimit), 3, 240 };
    Ubiquitty::Terminal successor;
    CHECK(restoreHandoverHistory(bytes->view(), successor, exact).has_value());
    for (const auto limit : std::array {
             HandoverHistoryLimits { exact.maxSectionBytes - 1, exact.maxCapacity, 3, 240 },
             HandoverHistoryLimits { exact.maxSectionBytes, exact.maxCapacity - 1, 3, 240 },
             HandoverHistoryLimits { exact.maxSectionBytes, exact.maxCapacity, 2, 240 },
             HandoverHistoryLimits { exact.maxSectionBytes, exact.maxCapacity, 3, 239 } }) {
        CHECK(restoreHandoverHistory(bytes->view(), successor, limit).error() == HandoverBodyError::OverCapacity);
        CHECK(encodeHandoverHistory(source, limit).error() == HandoverBodyError::OverCapacity);
    }
    auto* const owner = successor.lineStore.get();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        CHECK(restoreHandoverHistory(bytes->view(), successor, exact).error() == HandoverBodyError::AllocationRefused);
    }
    CHECK(successor.lineStore.get() == owner);
}

TEST(historyHandoverDistinguishesAbsentEmptyAndExhaustedStores)
{
    Ubiquitty::Terminal source;
    source.historyEpoch = 7;
    source.historyEpochBase = 500;
    const auto absent = encodeHandoverHistory(source, historyLimits);
    REQUIRE(absent);
    Ubiquitty::Terminal successor;
    historyFixture(successor, 3);
    REQUIRE(restoreHandoverHistory(absent->view(), successor, historyLimits));
    CHECK(!successor.lineStore);
    CHECK_EQ(successor.historyEpochBase, 500u);
    source.lineStore = std::make_unique<Ubiquitty::LineStore>();
    source.lineStore->init(32);
    source.scrollbackCapacity = 32;
    Ubiquitty::LineStore::Continuation continuation {
        { std::numeric_limits<std::uint64_t>::max(), Ubiquitty::LineStore::LineContinuity::StartsLine },
        std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<long long>::max()
    };
    REQUIRE(source.lineStore->restore({}, continuation));
    const auto empty = encodeHandoverHistory(source, historyLimits);
    REQUIRE(empty);
    REQUIRE(restoreHandoverHistory(empty->view(), successor, historyLimits));
    REQUIRE(successor.lineStore);
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'x';
    successor.lineStore->append(&cell, 1, 0, 0, false, false);
    CHECK_EQ(successor.lineStore->count(), 0);
    CHECK_EQ(successor.lineStore->totalAppended(), std::numeric_limits<long long>::max());
    CHECK_EQ(successor.lineStore->headLineNumber(), std::numeric_limits<std::uint64_t>::max());
}

namespace {

std::vector<std::uint8_t> screenCheckpoint(const Ubiquitty::Terminal& terminal)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> bytes;
    CHECK(publisher.capture(terminal));
    CHECK(publisher.checkpoint(terminal, 1, bytes, baseline));
    return bytes;
}

void spentLinkFixture(Ubiquitty::Terminal& terminal)
{
    // Stand in for the already spent sequential prefix, then use the real
    // parser, census and ring eviction for the boundary that matters.
    terminal.linkNextID = 65535;
    UbiquittyTest::feed(terminal, "\033]8;;https://example.test/original\033\\kept\033]8;;\033\\");
    for (int index = 0; index < 65; ++index)
        (void)Ubiquitty::Attributes::Hyperlinks::intern(&terminal, {}, "https://example.test/retired-" + std::to_string(index));
    terminal.currentLinkID = 0;
}

} // namespace

TEST(linkHandoverKeepsRetiredCellIDsOutOfTheSuccessorsAllocator)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    Ubiquitty::Terminal source;
    spentLinkFixture(source);
    REQUIRE(source.linkNextID == 0);
    REQUIRE(!source.linkFreeIDs.empty());
    REQUIRE(UbiquittyTest::cell(source, 0, 0)->linkID() == 65535);
    REQUIRE(Links::uri(&source, 65535) == nullptr);
    const auto checkpoint = screenCheckpoint(source);
    const auto bytes = encodeHandoverLinks(source, 512 * 1024);
    REQUIRE(bytes);

    // A replica checkpoint is insufficient for a successor: the live ring's
    // highest ID is no longer the allocator's high-water mark.
    Ubiquitty::Terminal checkpointOnly;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(checkpointOnly, checkpoint, replica) == ApplyOutcome::Applied);
    (void)Links::intern(&checkpointOnly, {}, "https://example.test/new-a");
    (void)Links::intern(&checkpointOnly, {}, "https://example.test/new-b");
    CHECK(Links::uri(&checkpointOnly, 65535) != nullptr);

    Ubiquitty::Terminal successor;
    REQUIRE(applyCheckpoint(successor, checkpoint, replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverLinks(bytes->view(), successor, 512 * 1024));
    const auto roundTrip = encodeHandoverLinks(successor, 512 * 1024);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(roundTrip->view(), bytes->view()));
    for (const auto* target : { "https://example.test/new-a", "https://example.test/new-b" })
        CHECK_EQ(Links::intern(&source, {}, target), Links::intern(&successor, {}, target));
    CHECK(Links::uri(&source, 65535) == nullptr);
    CHECK(Links::uri(&successor, 65535) == nullptr);
    CHECK_EQ(UbiquittyTest::cell(successor, 0, 0)->linkID(), 65535);
}

TEST(linkHandoverRejectsReclaimedIDsStillReferencedByScreensOrHistory)
{
    Ubiquitty::Terminal source;
    spentLinkFixture(source);
    const auto bytes = encodeHandoverLinks(source, 512 * 1024);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    auto* const oldTable = successor.linkTable.get();
    auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
    // The first reclaimed ID follows the fixed allocator header.
    writeLittle(changed, 15, 65535, 2);
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, successor, 512 * 1024).error() == HandoverBodyError::Malformed);
    CHECK(successor.linkTable.get() == oldTable);

    Ubiquitty::Terminal withHistory;
    historyFixture(withHistory, 1); // Every row cell carries ID 400.
    changed.assign(bytes->view().begin(), bytes->view().end());
    writeLittle(changed, 15, 400, 2);
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, withHistory, 512 * 1024).error() == HandoverBodyError::Malformed);
    source.linkFreeIDs.push_back(65535);
    CHECK(encodeHandoverLinks(source, 512 * 1024).error() == HandoverBodyError::Malformed);
}

TEST(linkHandoverPreservesKeyedDedupAndRefusesMalformedSections)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033]8;id=stable;https://example.test/one\033\\x\033]8;;\033\\");
    const auto bytes = encodeHandoverLinks(source, 4096);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverLinks(bytes->view(), successor, bytes->size));
    CHECK_EQ(Links::intern(&source, "stable", "https://example.test/two"), Links::intern(&successor, "stable", "https://example.test/two"));
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverLinks(bytes->view().first(length), successor, 4096));
    CHECK(restoreHandoverLinks(bytes->view(), successor, bytes->size - 1).error() == HandoverBodyError::OverCapacity);
    auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
    changed[2] = 2; // A bool is not an open-ended integer.
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, successor, 4096).error() == HandoverBodyError::Malformed);
    changed.assign(bytes->view().begin(), bytes->view().end());
    writeLittle(changed, 7, Ubiquitty::VT::linkTableCapacity, 2);
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, successor, 4096).error() == HandoverBodyError::Malformed);
    changed.assign(bytes->view().begin(), bytes->view().end());
    changed[15] ^= 0x80; // A short target's dedup hash is fully verifiable.
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, successor, 4096).error() == HandoverBodyError::Malformed);
    changed.assign(bytes->view().begin(), bytes->view().end());
    writeLittle(changed, bytes->size - 18, 4097, 2);
    refreshImageDigest(changed);
    CHECK(restoreHandoverLinks(changed, successor, 4096).error() == HandoverBodyError::Malformed);
    source.linkReclaimBackoffMints = -1;
    CHECK(encodeHandoverLinks(source, 4096).error() == HandoverBodyError::Malformed);
}

TEST(linkHandoverPreservesOriginalHashesForClippedTargets)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    for (const auto capacity : { Ubiquitty::VT::maxLinkKeyLength, Ubiquitty::VT::maxLinkURILength }) {
        Ubiquitty::Terminal source;
        const std::string original(static_cast<std::size_t>(capacity + 8), 'x');
        const auto key = capacity == Ubiquitty::VT::maxLinkKeyLength ? std::string_view(original) : std::string_view {};
        const auto uri = key.empty() ? std::string_view(original) : "https://example.test/long-key";
        const auto initial = Links::intern(&source, key, uri);
        REQUIRE(initial != 0);
        const auto bytes = encodeHandoverLinks(source, 8192);
        REQUIRE(bytes);
        Ubiquitty::Terminal successor;
        REQUIRE(restoreHandoverLinks(bytes->view(), successor, 8192));
        const auto clipped = std::string_view(original).substr(0, static_cast<std::size_t>(capacity - 1));
        const auto nextKey = key.empty() ? std::string_view {} : clipped;
        const auto nextURI = key.empty() ? clipped : uri;
        const auto sourceID = Links::intern(&source, nextKey, nextURI);
        CHECK(sourceID != initial);
        CHECK_EQ(Links::intern(&successor, nextKey, nextURI), sourceID);
    }
}

namespace {

constexpr HandoverTextStoreLimits textLimits { 4 * 1024 * 1024, Ubiquitty::ClusterPool::maxEntries, Ubiquitty::RunTextStore::maxEntries, 1024 * 1024 };

void smallTextFixture(Ubiquitty::Terminal& terminal)
{
    for (std::uint32_t index = 0; index < 3; ++index) {
        const std::array<std::uint32_t, 2> text { 'a', 0x300 + index };
        (void)terminal.clusterPool.intern(text);
        (void)terminal.runTextStore.intern(text);
    }
    const std::array<std::uint64_t, 1> keep { 1u << 1 };
    terminal.runTextStore.retainOnly(keep);
}

} // namespace

TEST(textStoreHandoverContinuesAFullClusterPoolAndExactRunIDOrder)
{
    Ubiquitty::Terminal source;
    for (std::uint32_t index = 0; index < Ubiquitty::ClusterPool::maxEntries + 3u; ++index) {
        const std::array<std::uint32_t, 2> cluster { 'a', 0x1000 + index };
        (void)source.clusterPool.intern(cluster);
    }
    REQUIRE(source.clusterPool.isFull());
    REQUIRE(source.clusterPool.continuation().oldestSlot == 0);
    for (std::uint32_t index = 0; index < 6; ++index) {
        const std::array<std::uint32_t, 2> text { 'b', 0x300 + index };
        (void)source.runTextStore.intern(text);
    }
    std::array<std::uint64_t, 1> keep { (1u << 1) | (1u << 3) | (1u << 4) };
    source.runTextStore.retainOnly(keep);
    const std::array<std::uint32_t, 1> newText { 'z' };
    REQUIRE(source.runTextStore.intern(newText) == 6);
    keep[0] = 1u << 1;
    source.runTextStore.retainOnly(keep);
    const auto continuation = source.runTextStore.continuation();
    REQUIRE(continuation.slotCount == 6);
    REQUIRE(continuation.freeIDs.size() == 5);
    REQUIRE(continuation.freeIDs.back() == 6);
    const auto bytes = encodeHandoverTextStores(source, textLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverTextStores(bytes->view(), successor, textLimits));
    const auto roundTrip = encodeHandoverTextStores(successor, textLimits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(roundTrip->view(), bytes->view()));
    for (std::uint32_t index = 0; index < 7; ++index) {
        const std::array<std::uint32_t, 2> text { 'c', 0xA000 + index };
        CHECK_EQ(source.clusterPool.intern(text), successor.clusterPool.intern(text));
        CHECK_EQ(source.runTextStore.intern(text), successor.runTextStore.intern(text));
    }
    const auto afterSource = encodeHandoverTextStores(source, textLimits);
    const auto afterSuccessor = encodeHandoverTextStores(successor, textLimits);
    REQUIRE(afterSource && afterSuccessor);
    CHECK(std::ranges::equal(afterSource->view(), afterSuccessor->view()));
}

TEST(textStoreHandoverRetainsSweepThresholdAndEmptyTrailingSlots)
{
    Ubiquitty::Terminal source;
    const std::vector<std::uint32_t> text(4000, 'x');
    for (unsigned index = 0; index < 20; ++index)
        (void)source.runTextStore.intern(text);
    std::array<std::uint64_t, 1> keep {};
    for (unsigned id = 1; id <= 10; ++id)
        keep[0] |= std::uint64_t { 1 } << id;
    source.runTextStore.retainOnly(keep);
    REQUIRE(source.runTextStore.continuation().sweepThreshold == 80000);
    REQUIRE(source.runTextStore.continuation().slotCount == 20);
    const auto bytes = encodeHandoverTextStores(source, textLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverTextStores(bytes->view(), successor, textLimits));
    CHECK_EQ(successor.runTextStore.continuation().sweepThreshold, 80000u);
    CHECK_EQ(successor.runTextStore.continuation().slotCount, 20);
    CHECK_EQ(successor.runTextStore.intern(text), source.runTextStore.intern(text));
    CHECK(successor.runTextStore.restoreContinuation(successor.runTextStore.continuation()));
}

TEST(textStoreHandoverValidatesEveryAllocatorAndScalarBeforeMutation)
{
    Ubiquitty::Terminal source;
    smallTextFixture(source);
    const auto bytes = encodeHandoverTextStores(source, textLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    const std::array<std::uint32_t, 1> existing { 'q' };
    (void)successor.runTextStore.intern(existing);
    const auto before = encodeHandoverTextStores(successor, textLimits);
    REQUIRE(before);
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverTextStores(bytes->view().first(length), successor, textLimits));
    for (const auto& [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 6> {
             std::array<std::uint64_t, 3> { 6, 1, 2 }, // Nonfull cluster pool cannot have a recycling head.
             { 12, 0, 4 }, // No valid source has a zero sweep threshold.
             { 17, 0xD800, 4 }, // Surrogate, not a Unicode scalar.
             { bytes->size - 20, 2, 2 }, // Repeated free ID leaves another empty slot unaccounted for.
             { bytes->size - 18, 64, 1 }, // Run retry cadence is at most 63.
             { bytes->size - 17, 65, 1 } // Spec retry cadence is at most 64.
         }) {
        auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
        writeLittle(changed, static_cast<std::size_t>(offset), value, static_cast<std::size_t>(width));
        refreshImageDigest(changed);
        CHECK(restoreHandoverTextStores(changed, successor, textLimits).error() == HandoverBodyError::Malformed);
    }
    const auto after = encodeHandoverTextStores(successor, textLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    source.runTextReclaimBackoffInterns = -1;
    CHECK(encodeHandoverTextStores(source, textLimits).error() == HandoverBodyError::Malformed);
    source.runTextReclaimBackoffInterns = 0;
    source.textSizeReclaimBackoffInterns = 65;
    CHECK(encodeHandoverTextStores(source, textLimits).error() == HandoverBodyError::Malformed);
}

TEST(textStoreHandoverKeepsTheRetryCadenceAfterAnExhaustedRunIsErased)
{
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 70, 10);
    source.drawsSizedText = true;
    source.drawsProportionalText = true;
    const auto fullRun = std::string("\033]66;p=1:c=2;") + std::string(Ubiquitty::runTextMaximumCodePoints, 'a') + "\007\r\n";
    for (std::size_t index = 0; index < Ubiquitty::RunTextStore::maxLiveCodePoints / Ubiquitty::runTextMaximumCodePoints; ++index)
        UbiquittyTest::feed(source, fullRun);
    REQUIRE(source.runTextStore.liveCodePoints() == Ubiquitty::RunTextStore::maxLiveCodePoints);
    UbiquittyTest::feed(source, "\033]66;p=1:c=2;x\007");
    REQUIRE(source.runTextReclaimBackoffInterns == 63);
    // Erasing makes one old run reclaimable, but does not advance the retry cadence.
    UbiquittyTest::feed(source, "\033[H\033[2K\033[66;1H");
    const auto text = encodeHandoverTextStores(source, textLimits);
    const auto parser = encodeHandoverParser(source, 4096);
    REQUIRE(text && parser);
    Ubiquitty::Terminal successor;
    successor.drawsSizedText = true;
    successor.drawsProportionalText = true;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverTextStores(text->view(), successor, textLimits));
    REQUIRE(restoreHandoverParser(parser->view(), successor, 4096));
    CHECK_EQ(source.runTextReclaimBackoffInterns, successor.runTextReclaimBackoffInterns);
    UbiquittyTest::feed(source, "\033]66;p=1:c=2;y\007");
    UbiquittyTest::feed(successor, "\033]66;p=1:c=2;y\007");
    CHECK_EQ(source.runTextStore.liveCodePoints(), successor.runTextStore.liveCodePoints());
    CHECK_EQ(source.runTextReclaimBackoffInterns, successor.runTextReclaimBackoffInterns);
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
}

TEST(textStoreHandoverKeepsTheTextSizeRetryCadenceAfterErase)
{
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 20, 24);
    source.drawsSizedText = true;
    source.drawsProportionalText = true;
    for (unsigned cells = 1; cells <= 15; ++cells)
        UbiquittyTest::feed(source, "\033]66;p=1:c=" + std::to_string(cells) + ";x\007\r\n");
    REQUIRE(source.textSizeTable.liveCount() == 15);
    UbiquittyTest::feed(source, "\033]66;p=1:c=16;x\007");
    REQUIRE(source.textSizeReclaimBackoffInterns == 64);
    UbiquittyTest::feed(source, "\033[H\033[2K\033[18;1H");
    const auto text = encodeHandoverTextStores(source, textLimits);
    REQUIRE(text);
    Ubiquitty::Terminal successor;
    successor.drawsSizedText = true;
    successor.drawsProportionalText = true;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverTextStores(text->view(), successor, textLimits));
    CHECK_EQ(source.textSizeReclaimBackoffInterns, successor.textSizeReclaimBackoffInterns);
    UbiquittyTest::feed(source, "\033]66;p=1:c=16;y\007");
    UbiquittyTest::feed(successor, "\033]66;p=1:c=16;y\007");
    CHECK_EQ(source.textSizeReclaimBackoffInterns, successor.textSizeReclaimBackoffInterns);
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
}

TEST(linkHandoverKeepsTheRetryCadenceAfterAnExhaustedIDIsErased)
{
    namespace Links = Ubiquitty::Attributes::Hyperlinks;
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 256, 256);
    // Model the spent sequential prefix with every ID still referenced, then
    // use the real census and mint operation to reach its refusal/backoff.
    source.linkNextID = 0;
    for (std::uint32_t id = 1; id <= 65535; ++id) {
        Ubiquitty::Cell cell {};
        cell.codePoint = 'x';
        cell.setLinkID(static_cast<std::uint16_t>(id));
        source.grid->setCell(Ubiquitty::Row(static_cast<int>((id - 1) / 256)), Ubiquitty::Column(static_cast<int>((id - 1) % 256)), cell);
    }
    REQUIRE(Links::intern(&source, {}, "https://example.test/refused") == 0);
    REQUIRE(source.linkReclaimBackoffMints == 4096);
    source.grid->setCell(Ubiquitty::Row(0), Ubiquitty::Column(0), Ubiquitty::Cell {});
    const auto links = encodeHandoverLinks(source, 512 * 1024);
    REQUIRE(links);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverLinks(links->view(), successor, 512 * 1024));
    CHECK_EQ(source.linkReclaimBackoffMints, successor.linkReclaimBackoffMints);
    UbiquittyTest::feed(source, "\033]8;;https://example.test/next\033\\y");
    UbiquittyTest::feed(successor, "\033]8;;https://example.test/next\033\\y");
    CHECK_EQ(source.linkReclaimBackoffMints, successor.linkReclaimBackoffMints);
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
}

TEST(textStoreHandoverAppliesReceiverBoundsAndRefusesOutputAllocation)
{
    Ubiquitty::Terminal source;
    smallTextFixture(source);
    const auto bytes = encodeHandoverTextStores(source, textLimits);
    REQUIRE(bytes);
    const HandoverTextStoreLimits exact { bytes->size, 3, 3, 8 };
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverTextStores(bytes->view(), successor, exact));
    for (const auto limit : std::array {
             HandoverTextStoreLimits { exact.maxSectionBytes - 1, 3, 3, 8 },
             HandoverTextStoreLimits { exact.maxSectionBytes, 2, 3, 8 },
             HandoverTextStoreLimits { exact.maxSectionBytes, 3, 2, 8 },
             HandoverTextStoreLimits { exact.maxSectionBytes, 3, 3, 7 } }) {
        CHECK(restoreHandoverTextStores(bytes->view(), successor, limit).error() == HandoverBodyError::OverCapacity);
        CHECK(encodeHandoverTextStores(source, limit).error() == HandoverBodyError::OverCapacity);
    }
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        CHECK(encodeHandoverTextStores(source, exact).error() == HandoverBodyError::AllocationRefused);
    }
    const std::array<std::uint32_t, 1> invalid { 0x110000 };
    (void)source.runTextStore.intern(invalid);
    CHECK(encodeHandoverTextStores(source, textLimits).error() == HandoverBodyError::Malformed);
}

TEST(parserHandoverKeepsAnOpenGraphemeAndTheCheckpointREPRecord)
{
    const std::array<std::pair<std::string_view, std::string_view>, 3> sequences {
        std::pair<std::string_view, std::string_view> { "\xf0\x9f\x91\xa8\xe2\x80\x8d", "\xf0\x9f\x91\xa9" },
        { "\xf0\x9f\x87\xa6", "\xf0\x9f\x87\xa7" },
        { "e", "\xcc\x81\033[2b" }
    };
    for (const auto& [prefix, suffix] : sequences) {
        Ubiquitty::Terminal source;
        Ubiquitty::resize(source, 4, 20);
        UbiquittyTest::feed(source, prefix);
        REQUIRE(source.parser.clusterActive);
        const auto parser = encodeHandoverParser(source, 4096);
        const auto text = encodeHandoverTextStores(source, textLimits);
        REQUIRE(parser && text);
        const auto checkpoint = screenCheckpoint(source);
        Ubiquitty::Terminal successor;
        ScreenReplica replica;
        REQUIRE(applyCheckpoint(successor, checkpoint, replica) == ApplyOutcome::Applied);
        REQUIRE(restoreHandoverTextStores(text->view(), successor, textLimits));
        REQUIRE(restoreHandoverParser(parser->view(), successor, 4096));
        const auto roundTrip = encodeHandoverParser(successor, 4096);
        REQUIRE(roundTrip);
        CHECK(std::ranges::equal(roundTrip->view(), parser->view()));
        UbiquittyTest::feed(source, suffix);
        UbiquittyTest::feed(successor, suffix);
        CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
        CHECK_EQ(source.parser.clusterCodepointLength, successor.parser.clusterCodepointLength);
        CHECK_EQ(source.parser.clusterLast, successor.parser.clusterLast);
    }
}

TEST(parserHandoverPreservesVT52AndLegacyInputPolicy)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[?2l");
    source.parser.invalidUTF8Policy = Ubiquitty::Unicode::InvalidUTF8Policy::Drop;
    Ubiquitty::Unicode::encReset(&source.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    source.parser.parserInputIsUTF8 = false;
    const auto parser = encodeHandoverParser(source, 4096);
    REQUIRE(parser);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverParser(parser->view(), successor, 4096));
    CHECK(successor.parser.parseState == Ubiquitty::VT::Parser::State::VT52Ground);
    CHECK(successor.parser.inputEncoding.enc == Ubiquitty::Unicode::Encoding::Latin1);
    CHECK(successor.parser.invalidUTF8Policy == Ubiquitty::Unicode::InvalidUTF8Policy::Drop);
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "\033H\xa3");
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
}

TEST(parserHandoverRefusesInvalidStateWithoutChangingTheDestination)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "e");
    const auto bytes = encodeHandoverParser(source, 4096);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    UbiquittyTest::feed(successor, "q");
    const auto before = encodeHandoverParser(successor, 4096);
    REQUIRE(before);
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverParser(bytes->view().first(length), successor, 4096));
    for (const auto& [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 7> {
             std::array<std::uint64_t, 3> { 2, static_cast<std::uint64_t>(Ubiquitty::VT::Parser::State::CSI), 4 },
             { 6, 3, 1 }, // Unknown invalid-input policy.
             { 8, 10, 1 }, // Unknown encoding.
             { 15, 1, 1 }, // Partial legacy character.
             { 16, 2, 1 }, // Noncanonical cluster-active flag.
             { 34, 3, 1 }, // Impossible Indic chain state.
             { 44, Ubiquitty::ClusterPool::maxCodePoints + 1, 4 } }) {
        auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
        writeLittle(changed, static_cast<std::size_t>(offset), value, static_cast<std::size_t>(width));
        refreshImageDigest(changed);
        CHECK(restoreHandoverParser(changed, successor, 4096).error() == HandoverBodyError::Malformed);
    }
    const auto after = encodeHandoverParser(successor, 4096);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    CHECK(restoreHandoverParser(bytes->view(), successor, bytes->size - 1).error() == HandoverBodyError::OverCapacity);
}

TEST(parserHandoverDoesNotCompleteOpenSequencesOrPartialCharacters)
{
    Ubiquitty::Terminal source;
    const auto ground = encodeHandoverParser(source, 4096);
    REQUIRE(ground);
    UbiquittyTest::feed(source, "\033]2;unfinished");
    const auto state = source.parser.parseState;
    const auto length = source.parser.osc.length;
    CHECK(encodeHandoverParser(source, 4096).error() == HandoverBodyError::NotAtGround);
    CHECK(restoreHandoverParser(ground->view(), source, 4096).error() == HandoverBodyError::NotAtGround);
    CHECK(source.parser.parseState == state);
    CHECK_EQ(source.parser.osc.length, length);
    Ubiquitty::Terminal partial;
    UbiquittyTest::feed(partial, "\xf0\x9f");
    CHECK(encodeHandoverParser(partial, 4096).error() == HandoverBodyError::NotAtGround);
    CHECK(partial.parser.utf8.need != 0);
}

namespace {
constexpr HandoverVisualLimits visualLimits { 256 * 1024, Ubiquitty::Tek::tekMaxPrimitives };

void downloadFont(Ubiquitty::Terminal& terminal)
{
    UbiquittyTest::feed(terminal, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\\033(@");
}

bool sameVisualStores(const Ubiquitty::Terminal& left, const Ubiquitty::Terminal& right)
{
    const auto leftBytes = encodeHandoverVisualStores(left, visualLimits);
    const auto rightBytes = encodeHandoverVisualStores(right, visualLimits);
    return leftBytes && rightBytes && std::ranges::equal(leftBytes->view(), rightBytes->view());
}
}

TEST(visualHandoverPreservesDownloadedGlyphsAndFutureDesignation)
{
    Ubiquitty::Terminal source;
    downloadFont(source);
    REQUIRE(source.softFont && source.softFont->valid && source.softFont->glyphSet[1]);
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
    CHECK(source.softFont.get() != successor.softFont.get());
    CHECK(sameVisualStores(source, successor));
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "!A\033(Bx\033(@!");
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
    CHECK_EQ(UbiquittyTest::cell(successor, 0, 0)->codePoint, Ubiquitty::VT::drcsCodePointBase + 1);
    CHECK_EQ(successor.softFont->glyph[1][7], 0xff);
    CHECK_EQ(successor.softFont->serial, source.softFont->serial);
    // A smaller Pe=1 matrix retains untouched glyphs from the old download.
    UbiquittyTest::feed(source, "\033P1;2;1;4;0;0;6;0{@~???\033\\");
    REQUIRE(source.softFont->matrixWidth == 4);
    REQUIRE(source.softFont->glyph[1][7] == 0xff);
    const auto smaller = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(smaller);
    REQUIRE(restoreHandoverVisualStores(smaller->view(), successor, visualLimits));
    CHECK(sameVisualStores(source, successor));
}

TEST(visualHandoverKeepsDistinctTekFramesAndPartialCoordinateBytes)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[?38hold\033\014new\033b\033:\035\054\160");
    REQUIRE(source.isTekModeActive);
    REQUIRE(source.tek.displayCount == 3 && source.tek.primitiveCount == 3);
    REQUIRE(source.tek.coordHasHighY && source.tek.coordHasLowY);
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
    CHECK(sameVisualStores(source, successor));
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "\043\104\105\106\037!\033\014next");
    CHECK(sameVisualStores(source, successor));
    CHECK_EQ(successor.tek.displayCount, 6);
    CHECK_EQ(successor.tek.primitiveCount, 4);
    CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
}

TEST(visualHandoverPreservesTekExitAccumulatorGINAndEchoBypass)
{
    for (const auto& prefix : { std::string("\033[?3"), std::string("\033[") + std::string(300, '?') }) {
        Ubiquitty::Terminal source;
        UbiquittyTest::feed(source, "\033[?38hA");
        UbiquittyTest::feed(source, prefix);
        const auto bytes = encodeHandoverVisualStores(source, visualLimits);
        REQUIRE(bytes);
        Ubiquitty::Terminal successor;
        ScreenReplica replica;
        REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
        REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
        for (auto* terminal : { &source, &successor })
            UbiquittyTest::feed(*terminal, "8lZ");
        CHECK(source.isTekModeActive == successor.isTekModeActive);
        CHECK(sameVisualStores(source, successor));
        CHECK(screenCheckpoint(source) == screenCheckpoint(successor));
    }
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[?38h\033\032");
    Ubiquitty::tekUpdateGINPosition(source, 2049, 3001);
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
    std::array<std::array<char, 32>, 2> replies {};
    unsigned index = 0;
    for (auto* terminal : { &source, &successor }) {
        Ubiquitty::tekGINKey(*terminal, 'k', terminal->tek.GINX, terminal->tek.GINY);
        CHECK_EQ(UbiquittyTest::drain(*terminal, replies[index++]), 6);
        REQUIRE(terminal->tek.bypass);
    }
    CHECK(replies[0] == replies[1]);
    const auto bypass = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bypass);
    successor.tek.bypass = false;
    REQUIRE(restoreHandoverVisualStores(bypass->view(), successor, visualLimits));
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "echo\037A");
    CHECK(sameVisualStores(source, successor));
    CHECK_EQ(successor.tek.primitiveCount, 1);
}

TEST(visualHandoverRefusesMalformedOrOversizedStateBeforeAllocation)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[?38hA");
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    REQUIRE(bytes->size == 83); // Fixed header, one 15-byte primitive and digest.
    Ubiquitty::Terminal successor;
    downloadFont(successor);
    const auto before = encodeHandoverVisualStores(successor, visualLimits);
    REQUIRE(before);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverVisualStores(bytes->view().first(length), successor, visualLimits));
    for (const auto& [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 12> {
             std::array<std::uint64_t, 3> { 2, 2, 1 },
             { 3, 4097, 4 }, { 11, 4, 1 }, { 12, 4, 1 }, { 14, 4, 1 },
             { 20, 2, 1 }, { 22, 4096, 2 }, { 30, 1, 2 }, { 36, 4096, 4 },
             { 52, 3, 1 }, { 55, 4096, 2 }, { 63, 0xd800, 4 } }) {
        auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
        writeLittle(changed, offset, value, width);
        refreshImageDigest(changed);
        CHECK(restoreHandoverVisualStores(changed, successor, visualLimits).error() == HandoverBodyError::Malformed);
    }
    CHECK(restoreHandoverVisualStores(bytes->view(), successor, { bytes->size - 1, 4096 }).error() == HandoverBodyError::OverCapacity);
    CHECK(restoreHandoverVisualStores(bytes->view(), successor, { bytes->size, 0 }).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    const auto after = encodeHandoverVisualStores(successor, visualLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
}

TEST(visualHandoverAllocationsAreFallibleAndCommitBothStoresTogether)
{
    Ubiquitty::Terminal source;
    downloadFont(source);
    UbiquittyTest::feed(source, "\033[?38hsource");
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    UbiquittyTest::feed(successor, "\033[?38hprevious");
    for (const auto size : { sizeof(Ubiquitty::VT::SoftFont), sizeof(Ubiquitty::Tek::State) }) {
        const auto before = encodeHandoverVisualStores(successor, visualLimits);
        REQUIRE(before);
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(size);
            CHECK(restoreHandoverVisualStores(bytes->view(), successor, visualLimits).error() == HandoverBodyError::AllocationRefused);
        }
        const auto after = encodeHandoverVisualStores(successor, visualLimits);
        REQUIRE(after);
        CHECK(std::ranges::equal(before->view(), after->view()));
    }
    REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
    CHECK(sameVisualStores(source, successor));
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        CHECK(encodeHandoverVisualStores(source, visualLimits).error() == HandoverBodyError::AllocationRefused);
    }
    source.softFont.reset();
    const auto absent = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(absent);
    REQUIRE(restoreHandoverVisualStores(absent->view(), successor, visualLimits));
    CHECK(!successor.softFont);
    source.softFont = std::make_unique<Ubiquitty::VT::SoftFont>();
    const auto empty = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(empty);
    REQUIRE(restoreHandoverVisualStores(empty->view(), successor, visualLimits));
    CHECK(successor.softFont && !successor.softFont->valid);
}

TEST(visualHandoverPreservesFullTekListsAndRefusesInvalidFontMetadata)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[?38h");
    UbiquittyTest::feed(source, std::string(4097, 'A'));
    REQUIRE(source.tek.didDropPrimitive && source.tek.didSignalPrimitiveDrop);
    Ubiquitty::tekCommitFrame(source);
    const auto bytes = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(bytes);
    CHECK(encodeHandoverVisualStores(source, { visualLimits.maxSectionBytes, 4095 }).error() == HandoverBodyError::OverCapacity);
    Ubiquitty::Terminal successor;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(successor, screenCheckpoint(source), replica) == ApplyOutcome::Applied);
    REQUIRE(restoreHandoverVisualStores(bytes->view(), successor, visualLimits));
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "B\033\014C");
    CHECK(sameVisualStores(source, successor));
    UbiquittyTest::feed(source, "\033\003");
    downloadFont(source);
    const auto font = encodeHandoverVisualStores(source, visualLimits);
    REQUIRE(font);
    for (const auto& [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 6> {
             std::array<std::uint64_t, 3> { 3, 2, 1 }, { 4, 2, 1 }, { 5, 0x30, 1 },
             { 7, 17, 1 }, { 9, 0, 4 }, { 13, 2, 1 } }) {
        auto changed = std::vector<std::uint8_t>(font->view().begin(), font->view().end());
        writeLittle(changed, offset, value, width);
        refreshImageDigest(changed);
        CHECK(restoreHandoverVisualStores(changed, successor, visualLimits).error() == HandoverBodyError::Malformed);
    }
}

namespace {
constexpr HandoverSoundLimits soundLimits { 32 * 1024 * 1024, 28 * 1024 * 1024 };

bool sameSoundStores(const Ubiquitty::Terminal& left, const Ubiquitty::Terminal& right)
{
    const auto leftBytes = encodeHandoverSounds(left, soundLimits);
    const auto rightBytes = encodeHandoverSounds(right, soundLimits);
    return leftBytes && rightBytes && std::ranges::equal(leftBytes->view(), rightBytes->view());
}

void sampledSoundFixture(Ubiquitty::Terminal& terminal)
{
    auto& bank = Ubiquitty::ensureSampleBank(terminal);
    bank.slots[0] = { -32768, 0, 32767 };
    bank.tags[0] = 77;
    bank.serial = 9;
    UbiquittyTest::feed(terminal, "\033[1;1;1,~");
    Ubiquitty::ensurePendingSound(terminal).pop(); // Nonzero physical ring head.
    UbiquittyTest::feed(terminal, "\033[7:2:6:12:3:4:5:6:7000:30:2:63;32:1:2:3;1:6:14:255,~");
    bank.slots[0] = { 7, 8, 9 };
    bank.serial = 10;
    UbiquittyTest::feed(terminal, "\033[2:3:6;16;1,~");
    Ubiquitty::ensurePendingSound(terminal).queueResetStop();
}
}

TEST(soundHandoverPreservesCapturedPCMInsteadOfLookingUpTheMutableBank)
{
    Ubiquitty::Terminal source;
    sampledSoundFixture(source);
    REQUIRE(source.pendingSound->head == 1 && source.pendingSound->count == 2);
    const auto bytes = encodeHandoverSounds(source, soundLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverSounds(bytes->view(), successor, soundLimits));
    CHECK(sameSoundStores(source, successor));
    CHECK(source.sampleBank.get() != successor.sampleBank.get());
    CHECK(source.pendingSound->front()->request.samplePCM != successor.pendingSound->front()->request.samplePCM);
    const auto* first = successor.pendingSound->front();
    REQUIRE(first && first->request.sampleFrames == 3);
    CHECK_EQ(first->sampleSerial, 9u);
    CHECK_EQ(first->request.samplePCM[0], -32768);
    CHECK_EQ(first->request.samplePCM[2], 32767);
    CHECK_EQ(successor.sampleBank->slots[0][0], 7);
    CHECK_EQ(successor.sampleBank->slotForTag(77), 1);
    CHECK_EQ(successor.sampleBank->firstFreeSlot(), 2);
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "\033[4:1:6;1;1,~"); // Materializes the reset marker before this request.
    CHECK(sameSoundStores(source, successor));
    for (int index = 0; index < 4; ++index) {
        REQUIRE(source.pendingSound->front() && successor.pendingSound->front());
        if (index == 2) {
            CHECK_EQ(successor.pendingSound->front()->request.volume, 0);
            CHECK_EQ(successor.pendingSound->front()->request.channel, 0);
        }
        source.pendingSound->pop();
        successor.pendingSound->pop();
        CHECK(sameSoundStores(source, successor));
    }
    CHECK_EQ(successor.pendingSound->pcmBytes, 0u);
}

TEST(soundHandoverKeepsAFullRingAndItsDeferredRISStop)
{
    Ubiquitty::Terminal source;
    sampledSoundFixture(source);
    // Fill every physical slot before RIS; the stop is an extra bounded marker.
    source.pendingSound->hasResetStop = false;
    for (int index = source.pendingSound->count; index < Ubiquitty::VT::PendingSound::depth; ++index)
        UbiquittyTest::feed(source, "\033[1;1;1,~");
    UbiquittyTest::feed(source, "\033c");
    REQUIRE(!source.sampleBank && source.pendingSound);
    REQUIRE(source.pendingSound->count == 32 && source.pendingSound->hasResetStop);
    const auto bytes = encodeHandoverSounds(source, soundLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverSounds(bytes->view(), successor, soundLimits));
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "\033[2;1;1,~");
    CHECK(sameSoundStores(source, successor));
    REQUIRE(successor.pendingSound->count == 32);
    for (unsigned index = 0; index < 33; ++index) {
        REQUIRE(source.pendingSound->front() && successor.pendingSound->front());
        source.pendingSound->pop();
        successor.pendingSound->pop();
        CHECK(sameSoundStores(source, successor));
    }
    CHECK(!successor.pendingSound->front());
}

TEST(soundHandoverRefusesBadFramingRequestsAndCountsBeforeAllocation)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033[1;1;1,~");
    const auto bytes = encodeHandoverSounds(source, soundLimits);
    REQUIRE(bytes && bytes->size == 175);
    Ubiquitty::Terminal successor;
    sampledSoundFixture(successor);
    const auto before = encodeHandoverSounds(successor, soundLimits);
    REQUIRE(before);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!restoreHandoverSounds(bytes->view().first(length), successor, soundLimits));
    for (const auto& [offset, value, width] : std::array<std::array<std::uint64_t, 3>, 12> {
             std::array<std::uint64_t, 3> { 2, 2, 1 }, { 3, 2, 1 }, { 4, 32, 1 }, { 5, 33, 1 },
             { 6, 2, 1 }, { 7, 8, 1 }, { 8, 7, 1 }, { 10, 5001, 2 },
             { 135, 17, 4 }, { 149, 65, 1 }, { 151, 441001, 4 }, { 155, 1, 4 } }) {
        auto changed = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
        writeLittle(changed, offset, value, width);
        refreshImageDigest(changed);
        CHECK(restoreHandoverSounds(changed, successor, soundLimits).error() == HandoverBodyError::Malformed);
    }
    CHECK(restoreHandoverSounds(bytes->view(), successor, { bytes->size - 1, soundLimits.maxPCMBytes }).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    const auto after = encodeHandoverSounds(successor, soundLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
}

TEST(soundHandoverAdmitsCombinedPCMAndBothOwnersBeforeMutation)
{
    Ubiquitty::Terminal source;
    sampledSoundFixture(source);
    const auto bytes = encodeHandoverSounds(source, soundLimits);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    UbiquittyTest::feed(successor, "\033[1;1;1,~");
    const auto before = encodeHandoverSounds(successor, soundLimits);
    REQUIRE(before);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(restoreHandoverSounds(bytes->view(), successor, { soundLimits.maxSectionBytes, 17 }).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK(encodeHandoverSounds(source, { soundLimits.maxSectionBytes, 17 }).error() == HandoverBodyError::OverCapacity);
    for (const auto size : { sizeof(Ubiquitty::VT::SampleBank), sizeof(Ubiquitty::VT::PendingSound), 3 * sizeof(std::int16_t) }) {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(size);
        CHECK(restoreHandoverSounds(bytes->view(), successor, soundLimits).error() == HandoverBodyError::AllocationRefused);
    }
    const auto after = encodeHandoverSounds(successor, soundLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    REQUIRE(restoreHandoverSounds(bytes->view(), successor, { soundLimits.maxSectionBytes, 18 }));
    CHECK(sameSoundStores(source, successor));
    source.pendingSound->pcmBytes += 1;
    CHECK(encodeHandoverSounds(source, soundLimits).error() == HandoverBodyError::Malformed);
}

TEST(soundHandoverPreservesEmptyOwnersAndRejectsDuplicateBankTags)
{
    Ubiquitty::Terminal source;
    Ubiquitty::Terminal successor;
    sampledSoundFixture(successor);
    const auto absent = encodeHandoverSounds(source, soundLimits);
    REQUIRE(absent);
    REQUIRE(restoreHandoverSounds(absent->view(), successor, soundLimits));
    CHECK(!successor.sampleBank && !successor.pendingSound);
    auto& bank = Ubiquitty::ensureSampleBank(source);
    (void)Ubiquitty::ensurePendingSound(source);
    bank.serial = std::numeric_limits<std::uint32_t>::max();
    const auto empty = encodeHandoverSounds(source, soundLimits);
    REQUIRE(empty);
    REQUIRE(restoreHandoverSounds(empty->view(), successor, soundLimits));
    CHECK(successor.sampleBank && successor.pendingSound);
    for (auto* terminal : { &source, &successor })
        UbiquittyTest::feed(*terminal, "\033P1;0;0+s\033\\");
    CHECK_EQ(successor.sampleBank->serial, 0u);
    CHECK(sameSoundStores(source, successor));
    bank.slots[0] = { 1 };
    bank.slots[1] = { 2 };
    bank.tags[0] = 17;
    bank.tags[1] = 19;
    const auto tagged = encodeHandoverSounds(source, soundLimits);
    REQUIRE(tagged);
    auto duplicate = std::vector<std::uint8_t>(tagged->view().begin(), tagged->view().end());
    writeLittle(duplicate, 15, 17, 2);
    refreshImageDigest(duplicate);
    CHECK(restoreHandoverSounds(duplicate, successor, soundLimits).error() == HandoverBodyError::Malformed);
    bank.tags[1] = 17;
    CHECK(encodeHandoverSounds(source, soundLimits).error() == HandoverBodyError::Malformed);
}

TEST(soundHandoverKeepsTheIndependentPendingPCMCeiling)
{
    Ubiquitty::Terminal source;
    auto& pending = Ubiquitty::ensurePendingSound(source);
    std::vector<std::int16_t> pcm(Ubiquitty::VT::sampleMaxFrames, -1234);
    Ubiquitty::PAL::PlayTonesRequest request;
    request.wave = 6;
    request.volume = 1;
    request.count = 1;
    request.sampleSlot = 1;
    request.sampleFrames = static_cast<std::uint32_t>(pcm.size());
    request.samplePCM = pcm.data();
    for (unsigned index = 0; index < 4; ++index)
        REQUIRE(pending.push(request, index) == Ubiquitty::VT::PendingSoundAdmission::Queued);
    REQUIRE(pending.push(request, 4) == Ubiquitty::VT::PendingSoundAdmission::OverLimit);
    const auto bytes = encodeHandoverSounds(source, soundLimits);
    REQUIRE(bytes);
    constexpr std::size_t entryOffset = 7;
    const std::size_t entryBytes = 152 + pcm.size() * sizeof(std::int16_t);
    REQUIRE(bytes->size == entryOffset + 4 * entryBytes + 16);
    auto oversized = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end() - 16);
    oversized.insert(oversized.end(), bytes->view().begin() + entryOffset, bytes->view().begin() + entryOffset + entryBytes);
    oversized.resize(oversized.size() + 16);
    writeLittle(oversized, 5, 5, 1);
    refreshImageDigest(oversized);
    Ubiquitty::Terminal successor;
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(restoreHandoverSounds(oversized, successor, soundLimits).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK(!successor.pendingSound && !successor.sampleBank);
    REQUIRE(restoreHandoverSounds(bytes->view(), successor, soundLimits));
    CHECK(sameSoundStores(source, successor));
    auto& fifth = pending.requests[4];
    fifth.request = request;
    fifth.pcm = std::make_unique<std::int16_t[]>(pcm.size());
    std::memcpy(fifth.pcm.get(), pcm.data(), pcm.size() * sizeof(std::int16_t));
    fifth.request.samplePCM = fifth.pcm.get();
    ++pending.count;
    pending.pcmBytes += pcm.size() * sizeof(std::int16_t);
    CHECK(encodeHandoverSounds(source, soundLimits).error() == HandoverBodyError::OverCapacity);
}

namespace {
constexpr std::size_t hostStateLimit = 5 * 1024 * 1024;

std::string replies(Ubiquitty::Terminal& terminal)
{
    std::string output;
    std::array<char, 17> chunk {};
    while (const auto count = UbiquittyTest::drain(terminal, chunk))
        output.append(chunk.data(), count);
    return output;
}

/// Allows every effect, so a Remote terminal sends the answerback a handover
/// is asked to preserve.
///
/// With no policy the terminal answers PAL's Remote default, which refuses an
/// answerback. The policy is host-owned and stays out of the body, so source
/// and successor each install this one.
class AnsweringPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    /// Allows the request.
    ///
    /// - Returns: Allow, whatever is asked.
    Ubiquitty::CapabilityDecision check(Ubiquitty::Capability, Ubiquitty::CapabilityOrigin, std::string_view) override { return Ubiquitty::CapabilityDecision::Allow; }
};
}

// The sequence gate rides in the host state a local share carries. A gate
// holding more entries than its lists can, or a DCS kind past the last one, is
// not carried and not restored: a successor never installs a policy the
// window could not have held.
TEST(hostStateHandoverRefusesAGateItsListsCannotHold)
{
    for (int fault = 0; fault < 3; ++fault) {
        Ubiquitty::Terminal source;
        switch (fault) {
        case 0:
            source.sequenceGate.oscDenyCount = Ubiquitty::OSC::oscListCapacity + 1;
            break;
        case 1:
            source.sequenceGate.apcAllowCount = Ubiquitty::OSC::apcListCapacity + 1;
            break;
        case 2:
            source.sequenceGate.dcsDeny[0] = static_cast<Ubiquitty::OSC::DCSKind>(Ubiquitty::OSC::lastDCSKindValue + 1);
            source.sequenceGate.dcsDenyCount = 1;
            break;
        }
        const auto bytes = encodeHandoverHostState(source, hostStateLimit);
        Ubiquitty::Terminal successor;
        CHECK_MSG(!bytes || !restoreHandoverHostState(bytes->view(), successor, hostStateLimit),
            "a gate past its lists' capacity was carried and restored");
    }
}

TEST(hostStateHandoverPreservesPolicyAndOrderedUnsubmittedReplies)
{
    AnsweringPolicy answering;
    Ubiquitty::Terminal source;
    source.capabilityPolicy = &answering;
    source.answerback = "host continuation";
    source.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
    source.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
    source.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    source.sequenceGate.oscDeny[0] = 2;
    source.sequenceGate.oscDenyCount = 1;
    source.sequenceGate.hasDCSDenyOverflow = true;
    source.sequenceGate.apcAllow[0] = 'G';
    source.sequenceGate.apcAllowCount = 1;
    source.sequenceGate.apcDeny[0] = 'X';
    source.sequenceGate.apcDenyCount = 1;
    source.cellPixelWidth = 9;
    source.cellPixelHeight = 18;
    source.commandStartedGeneration = 37;
    source.commandCompletedGeneration = 30;
    source.commandExitCodeRing[6] = 12;
    source.commandDurationTicksRing[6] = 947;
    source.commandLine = "printf continued";
    source.reportedDirectory = "/test/reported";
    source.sampledDirectory = "/test/sampled";
    source.reportingProcessGroup = 41;
    source.foregroundProcessGroup = 42;
    source.bellPending = true;
    source.attentionGeneration = 91;
    source.outputLength = 3;
    source.outputBuffer[0] = 'A';
    source.outputBuffer[1] = 0;
    source.outputBuffer[2] = 'B';
    REQUIRE(source.bulkOutput.append("bulk-tail", Ubiquitty::VT::maxBulkOutputBytes));
    REQUIRE(source.extendedClipboardServedAliases.append("application/test", Ubiquitty::VT::maxExtendedClipboardAliasBytes));
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    CHECK(!successor.capabilityPolicy.installed());
    successor.capabilityPolicy = &answering;
    const auto encodedAgain = encodeHandoverHostState(successor, hostStateLimit);
    REQUIRE(encodedAgain);
    CHECK(std::ranges::equal(bytes->view(), encodedAgain->view()));
    CHECK(Ubiquitty::OSC::apcAllowed(successor, 'G'));
    CHECK(!Ubiquitty::OSC::apcAllowed(successor, 'X'));
    CHECK(!Ubiquitty::OSC::apcAllowed(successor, 'Y'));
    CHECK(replies(source) == std::string("A\0Bbulk-tail", 12));
    CHECK(replies(successor) == std::string("A\0Bbulk-tail", 12));
    UbiquittyTest::feed(source, "\033]2;denied\007\005\033[16t");
    UbiquittyTest::feed(successor, "\033]2;denied\007\005\033[16t");
    CHECK(source.titleUTF8.empty() && successor.titleUTF8.empty());
    const auto nextReplies = replies(source);
    CHECK(nextReplies.find("host continuation") != std::string::npos);
    CHECK(replies(successor) == nextReplies);
    CHECK_EQ(successor.commandExitCodeRing[6], 12);
    CHECK_EQ(successor.commandDurationTicksRing[6], 947u);
    CHECK_EQ(successor.reportingProcessGroup, 41);
    CHECK_EQ(successor.attentionGeneration, 91u);
    CHECK(std::string_view(successor.extendedClipboardServedAliases.data, successor.extendedClipboardServedAliases.length) == "application/test");
}

TEST(hostStateHandoverPreservesFontExhaustionAndTypographyEpoch)
{
    Ubiquitty::Terminal source;
    Ubiquitty::OSC::FontRequestQueue::Continuation font { "Test Mono", 12.5,
        UINT64_MAX, UINT64_MAX, UINT64_MAX - 1 };
    REQUIRE(source.fontRequests.restoreContinuation(font));
    auto facts = std::make_shared<Ubiquitty::RichText::TypographyFacts>();
    facts->rendererRevision = 0x123456789abcdef0ULL;
    facts->rendersBlocks = 1;
    facts->fontCatalogScope = Ubiquitty::RichText::FontCatalogScope::System;
    facts->cellWidth = 512;
    facts->cellHeight = 1024;
    facts->faceCount = 1;
    facts->faces[0].lineHeight = 2100;
    facts->faces[0].name.fill('N');
    facts->faces[0].nameLength = Ubiquitty::RichText::maxFamilyNameBytes;
    facts->faces[0].advances[17] = 1733;
    source.typography.facts = facts;
    source.typography.epoch = 0x76543210;
    source.typography.subscriptionID = 321;
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    CHECK_EQ(successor.fontRequests.generation(), UINT64_MAX);
    CHECK(successor.fontRequests.actualFamily() == "Test Mono");
    CHECK(successor.fontRequests.actualSizePoints() == 12.5);
    CHECK(successor.fontRequests.canPublishActualFamily());
    CHECK(!source.fontRequests.append("Other", 14, 0));
    CHECK(!successor.fontRequests.append("Other", 14, 0));
    REQUIRE(successor.typography.facts);
    CHECK(*source.typography.facts == *successor.typography.facts);
    CHECK(source.typography.facts.get() != successor.typography.facts.get());
    CHECK_EQ(successor.typography.epoch, source.typography.epoch);
    CHECK_EQ(successor.typography.subscriptionID, source.typography.subscriptionID);
}

// A host reads only the state format it writes: a host-state section of an
// earlier version is refused rather than read in the current layout.
TEST(anEarlierHostStateVersionIsRefused)
{
    Ubiquitty::Terminal source;
    const auto encoded = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(encoded);
    std::vector<std::uint8_t> earlier(encoded->view().begin(), encoded->view().end());
    earlier[0] = static_cast<std::uint8_t>(oldestHandoverBodyVersion - 1);
    earlier[1] = 0;
    refreshImageDigest(earlier);
    Ubiquitty::Terminal successor;
    const auto refused = restoreHandoverHostState(earlier, successor, hostStateLimit);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::Malformed);
}

TEST(hostStateHandoverPreservesRecordedSlotsAndLatentColorChannels)
{
    Ubiquitty::Terminal source;
    source.progressState = 4;
    source.progressValue = 75;
    source.tabStatusSet = 0;
    source.tabStatusR = 93;
    source.tabColorSet = 0;
    source.tabColorBlue = 104;
    source.badgeText = "host badge";
    source.currentFileURL = "file:///test/document";
    source.hasUnseenNotification = true;
    source.unseenNotificationTitle = "test";
    source.unseenNotificationBody = "pending";
    source.userVariables[7] = { "seventh", "kept", true };
    source.userVariables[31] = { "last", "also kept", true };
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    CHECK(!successor.userVariables[0].used);
    CHECK(successor.userVariables[7].name == "seventh");
    CHECK(successor.userVariables[31].value == "also kept");
    CHECK_EQ(successor.progressState, 4);
    CHECK_EQ(successor.progressValue, 75);
    CHECK_EQ(successor.tabStatusSet, 0);
    CHECK_EQ(successor.tabStatusR, 93);
    CHECK_EQ(successor.tabColorBlue, 104);
    CHECK(successor.currentFileURL == source.currentFileURL);
    CHECK(successor.unseenNotificationBody == "pending");
    // The next insert must choose the same free physical slot on both hosts.
    UbiquittyTest::feed(source, "\033]1337;SetUserVar=new=bmV3\007");
    UbiquittyTest::feed(successor, "\033]1337;SetUserVar=new=bmV3\007");
    CHECK(source.userVariables[0].name == "new");
    CHECK(successor.userVariables[0].name == "new");
    const auto before = encodeHandoverHostState(source, hostStateLimit);
    const auto after = encodeHandoverHostState(successor, hostStateLimit);
    REQUIRE(before && after);
    CHECK(std::ranges::equal(before->view(), after->view()));
}

TEST(hostStateHandoverPreservesTriggerProgramsAndPhysicalHighlightRing)
{
    Ubiquitty::Terminal source;
    source.triggersEngine = std::make_unique<Ubiquitty::Triggers::Engine>();
    REQUIRE(source.triggersEngine->addRule("^error[0-9]+$", Ubiquitty::Triggers::Action::Highlight, "orange", Ubiquitty::CaseSensitivity::Insensitive));
    REQUIRE(source.triggersEngine->addRule(".*", Ubiquitty::Triggers::Action::SetTitle, "matched", Ubiquitty::CaseSensitivity::Sensitive));
    REQUIRE(source.triggersEngine->addCharacterClass("word", "[a-z]+", Ubiquitty::CaseSensitivity::Insensitive));
    source.triggerHighlightHead = 63;
    source.triggerHighlightCount = 2;
    source.triggerHighlights[63] = { 120, 2, 9 };
    source.triggerHighlights[0] = { 125, 0, 2 };
    source.triggerWrapEvaluatedRow = 17;
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    REQUIRE(successor.triggersEngine);
    std::array<Ubiquitty::Triggers::Matched, 4> before {};
    std::array<Ubiquitty::Triggers::Matched, 4> after {};
    CHECK_EQ(source.triggersEngine->evaluateLine("ERROR123", before), 2);
    CHECK_EQ(successor.triggersEngine->evaluateLine("ERROR123", after), 2);
    for (unsigned index = 0; index < 2; ++index) {
        CHECK(before[index].action == after[index].action);
        CHECK(std::string(before[index].arg) == std::string(after[index].arg));
        CHECK_EQ(before[index].column0, after[index].column0);
        CHECK_EQ(before[index].column1, after[index].column1);
    }
    const auto selection = successor.triggersEngine->expandSelection("before WORD after", 9);
    REQUIRE(selection);
    CHECK_EQ(selection->begin, 7);
    CHECK_EQ(selection->end, 10);
    CHECK_EQ(successor.triggerHighlightHead, 63);
    CHECK_EQ(successor.triggerHighlightCount, 2);
    CHECK_EQ(successor.triggerHighlights[63].absoluteRow, 120);
    CHECK_EQ(successor.triggerHighlights[0].column1, 2);
    CHECK_EQ(successor.triggerWrapEvaluatedRow, 17);
}

TEST(hostStateHandoverPreservesStableIntegrationButNotBorrowedCallbacks)
{
    Ubiquitty::Terminal source;
    source.integration.tmux.enabled = true;
    source.integration.tmux.autoDetect = true;
    source.integration.tmux.atLineStart = false;
    source.integration.tmux.activePaneID = 731;
    source.integration.traceBytes[0] = 0x1b;
    source.integration.traceLength = 1;
    int sourceContext = 1;
    int successorContext = 2;
    source.integration.paneRouter.context = &sourceContext;
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    successor.integration.paneRouter.context = &successorContext;
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    CHECK(successor.integration.tmux.enabled && successor.integration.tmux.autoDetect);
    CHECK(!successor.integration.tmux.atLineStart);
    CHECK_EQ(successor.integration.tmux.activePaneID, 731);
    CHECK_EQ(successor.integration.traceBytes[0], 0x1b);
    CHECK_EQ(successor.integration.traceLength, 1);
    CHECK(successor.integration.paneRouter.context == &successorContext);
}

TEST(hostStateHandoverRefusesMalformedOrOversizedStateBeforeMutation)
{
    Ubiquitty::Terminal source;
    source.answerback = "source";
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    successor.answerback = "untouched";
    for (std::size_t size = 0; size < bytes->size; ++size)
        CHECK(!restoreHandoverHostState(bytes->view().first(size), successor, hostStateLimit));
    auto invalid = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
    invalid[6] = 2; // First Boolean follows version and triggerWrapEvaluatedRow.
    refreshImageDigest(invalid);
    CHECK(restoreHandoverHostState(invalid, successor, hostStateLimit).error() == HandoverBodyError::Malformed);
    CHECK(restoreHandoverHostState(bytes->view(), successor, bytes->size - 1).error() == HandoverBodyError::OverCapacity);
    CHECK(successor.answerback == "untouched");
    source.answerback.resize(Ubiquitty::Terminal::maxAnswerbackLength + 1);
    CHECK(!encodeHandoverHostState(source, hostStateLimit));
    source.answerback.clear();
    source.outputLength = 257;
    CHECK(!encodeHandoverHostState(source, hostStateLimit));
    source.outputLength = 0;
    REQUIRE(source.fontRequests.append("pending", 12, 0));
    CHECK(encodeHandoverHostState(source, hostStateLimit).error() == HandoverBodyError::NotAtGround);
}

TEST(hostStateHandoverAllocationRefusalLeavesEveryDestinationOwner)
{
    Ubiquitty::Terminal source;
    source.triggersEngine = std::make_unique<Ubiquitty::Triggers::Engine>();
    REQUIRE(source.triggersEngine->addRule("a", Ubiquitty::Triggers::Action::Notify, "source", Ubiquitty::CaseSensitivity::Sensitive));
    source.answerback = "source";
    REQUIRE(source.bulkOutput.append("new tail", Ubiquitty::VT::maxBulkOutputBytes));
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    successor.answerback = "destination";
    REQUIRE(successor.bulkOutput.append("old tail", Ubiquitty::VT::maxBulkOutputBytes));
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(Ubiquitty::Triggers::Engine));
        CHECK(restoreHandoverHostState(bytes->view(), successor, hostStateLimit).error() == HandoverBodyError::AllocationRefused);
    }
    CHECK(successor.answerback == "destination");
    CHECK(std::string_view(successor.bulkOutput.data, successor.bulkOutput.length) == "old tail");
    CHECK(!successor.triggersEngine);
    REQUIRE(restoreHandoverHostState(bytes->view(), successor, hostStateLimit));
    CHECK(successor.answerback == "source");
    CHECK(std::string_view(successor.bulkOutput.data, successor.bulkOutput.length) == "new tail");
}

TEST(restoredTriggerProgramsRefuseMissingTerminatorsAndInvalidOpcodes)
{
    Ubiquitty::Triggers::Engine engine;
    Ubiquitty::Triggers::Program program {};
    REQUIRE(Ubiquitty::Triggers::compile("x+", Ubiquitty::CaseSensitivity::Sensitive, &program));
    Ubiquitty::Triggers::Rule rule {};
    rule.action = Ubiquitty::Triggers::Action::Notify;
    auto invalid = program;
    invalid.tokenCount = Ubiquitty::Triggers::maxPatternLength;
    CHECK(!engine.restoreRule(rule, invalid));
    invalid = program;
    invalid.tokens[0].op = Ubiquitty::Triggers::Program::TokenOperation::End;
    CHECK(!engine.restoreRule(rule, invalid));
    invalid = program;
    invalid.tokens[program.tokenCount].op = Ubiquitty::Triggers::Program::TokenOperation::Literal;
    CHECK(!engine.restoreRule(rule, invalid));
    invalid = program;
    invalid.tokens[0].quant = static_cast<Ubiquitty::Triggers::Program::TokenQuantifier>(99);
    CHECK(!engine.restoreRule(rule, invalid));
    CHECK_EQ(engine.ruleCount(), 0);
    REQUIRE(engine.restoreRule(rule, program));
    CHECK_EQ(engine.ruleCount(), 1);
}

namespace {
constexpr HandoverTerminalLimits terminalLimits {
    64 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, hostStateLimit, 256 * 1024, 4096,
    imageLimits, historyLimits, textLimits, visualLimits, soundLimits
};

constexpr HandoverPaneLimits paneLimits {
    terminalLimits, { 1024 * 1024, 65536, 4096, 4096, 4096 },
    72 * 1024 * 1024, 5 * 1024 * 1024, 1024 * 1024, 4 * 1024 * 1024, 32 * 1024 * 1024, 64
};

struct HandoverInputWriter final : InputWriter {
    std::vector<std::uint8_t> bytes;
    WriteStatus write(ID128, const InputIntent& intent) override
    {
        if (const auto* raw = std::get_if<RawInput>(&intent))
            bytes.insert(bytes.end(), raw->bytes.begin(), raw->bytes.end());
        return WriteStatus::Written;
    }
};

void joinedTerminalFixture(Ubiquitty::Terminal& terminal)
{
    imageFixture(terminal);
    for (int index = 0; index < 24; ++index)
        UbiquittyTest::feed(terminal, "history line\r\n");
    UbiquittyTest::feed(terminal, "\033]133;A\007prompt\033]133;B\007\033]7;file://host/test/directory\007"
                                  "\033]1337;SetUserVar=build=cmVsZWFzZQ==\007"
                                  "\033]8;id=original;https://example.test/original\033\\linked\033]8;;\033\\"
                                  "\033P0;0;0;8;0;0;8;0{@~~~~\033\\"
                                  "\033]9;4;1;37\007");
    sampledSoundFixture(terminal);
    terminal.answerback = "continued host";
    terminal.sequenceGate.oscDeny[0] = 2;
    terminal.sequenceGate.oscDenyCount = 1;
    terminal.richTextBlocks.resumeSerials(731);
    UbiquittyTest::feed(terminal, "x");
}

std::array<std::vector<std::uint8_t>, 10> splitTerminalRecord(std::span<const std::uint8_t> bytes)
{
    WireReader reader(bytes);
    (void)reader.u16();
    (void)reader.u32();
    (void)reader.u32();
    std::array<std::vector<std::uint8_t>, 10> sections;
    for (auto& section : sections) {
        const auto size = reader.u32();
        const auto part = reader.take(size);
        section.assign(part.begin(), part.end());
    }
    CHECK(reader.ok());
    return sections;
}

std::vector<std::uint8_t> joinTerminalRecord(const std::array<std::vector<std::uint8_t>, 10>& sections, std::uint32_t richTextSerial,
    std::uint32_t richTextStorageLimit = Ubiquitty::RichText::maxRetainedBytes, std::uint16_t version = handoverBodyVersion)
{
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writer.u16(version);
    writer.u32(richTextSerial);
    writer.u32(richTextStorageLimit);
    for (const auto& section : sections) {
        writer.u32(static_cast<std::uint32_t>(section.size()));
        writer.raw(section);
    }
    bytes.resize(bytes.size() + 16);
    refreshImageDigest(bytes);
    return bytes;
}
}

TEST(terminalHandoverCarriesTheHostRichTextStorageQuota)
{
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 6, 40);
    // The sender's unused source capacity is not part of replicated content;
    // the byte quota remains identical even when decoding allocates less.
    UbiquittyTest::feed(source, "\033]7404;w=10:h=1:C=1:i=1;{\\rtf1 source}\033\\");
    REQUIRE(!source.richTextBlocks.isEmpty());
    const auto quota = source.richTextBlocks.storageBytes();
    REQUIRE(source.richTextBlocks.setStorageLimit(quota));
    const auto bytes = encodeHandoverTerminal(source, 17, terminalLimits);
    REQUIRE(bytes);
    auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
    REQUIRE(restored);
    auto& successor = *restored->terminal;
    CHECK_EQ(successor.richTextBlocks.storageLimit(), quota);
    CHECK(successor.richTextBlocks.storageBytes() <= source.richTextBlocks.storageBytes());
    CHECK(successor.richTextBlocks == source.richTextBlocks);
    const auto held = successor.richTextBlocks;
    UbiquittyTest::feed(successor, "\033[3;1H\033]7404;w=20:h=1:i=2;{\\rtf1 " + std::string(4096, 'x') + "}\033\\");
    CHECK(successor.richTextBlocks == held);

    const auto sections = splitTerminalRecord(bytes->view());
    const auto insufficient = joinTerminalRecord(sections, source.richTextBlocks.nextSerial(), 0);
    const auto refused = decodeHandoverTerminal(insufficient, terminalLimits);
    CHECK(!refused && refused.error() == HandoverBodyError::Malformed);
    const auto oversized = joinTerminalRecord(sections, source.richTextBlocks.nextSerial(), Ubiquitty::RichText::maxRetainedBytes + 1);
    const auto excessive = decodeHandoverTerminal(oversized, terminalLimits);
    CHECK(!excessive && excessive.error() == HandoverBodyError::OverCapacity);
}

TEST(terminalHandoverPreservesAZeroRichTextQuotaWithoutContent)
{
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 6, 40);
    REQUIRE(source.richTextBlocks.setStorageLimit(0));
    const auto bytes = encodeHandoverTerminal(source, 18, terminalLimits);
    REQUIRE(bytes);
    auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->terminal->richTextBlocks.storageLimit(), std::size_t { 0 });
    UbiquittyTest::feed(*restored->terminal, "\033]7404;w=10:h=1;{\\rtf1 refused}\033\\");
    CHECK(restored->terminal->richTextBlocks.isEmpty());
}

// A terminal record below the release floor is refused with or without
// rich text; no placement is read under another grammar.
TEST(anEarlierTerminalRecordVersionIsRefused)
{
    for (const std::string_view text : { std::string_view("carried ordinary terminal text"),
             std::string_view("\033]7404;w=10:h=1:C=1:i=1:D=19:M=1:g=1:v=1;{\\rtf1 hidden\\par shown}\033\\") }) {
        Ubiquitty::Terminal source;
        Ubiquitty::resize(source, 6, 40);
        UbiquittyTest::feed(source, std::string(text));
        const auto bytes = encodeHandoverTerminal(source, 19, terminalLimits);
        REQUIRE(bytes);
        const auto sections = splitTerminalRecord(bytes->view());
        for (const std::uint16_t version : { std::uint16_t { 33 }, static_cast<std::uint16_t>(oldestHandoverBodyVersion - 1) }) {
            const auto refused = decodeHandoverTerminal(joinTerminalRecord(sections, source.richTextBlocks.nextSerial(), Ubiquitty::RichText::maxRetainedBytes, version), terminalLimits);
            REQUIRE(!refused);
            CHECK(refused.error() == HandoverBodyError::Malformed);
        }
    }
}

TEST(aTerminalRecordCannotMixHandoverGrammars)
{
    Ubiquitty::Terminal source;
    const auto bytes = encodeHandoverTerminal(source, 19, terminalLimits);
    REQUIRE(bytes);
    const auto original = splitTerminalRecord(bytes->view());
    for (std::size_t index = 1; index < original.size(); ++index) {
        auto sections = original;
        const auto other = handoverBodyVersion == lastPublishedHandoverBodyVersion ? handoverBodyVersion + 1 : lastPublishedHandoverBodyVersion;
        writeLittle(sections[index], 0, other, 2);
        refreshImageDigest(sections[index]);
        const auto result = decodeHandoverTerminal(joinTerminalRecord(sections, source.richTextBlocks.nextSerial()), terminalLimits);
        REQUIRE(!result);
        CHECK(result.error() == HandoverBodyError::Malformed);
    }
}

TEST(joinedTerminalHandoverPreservesOwnersAndFutureProgramBytes)
{
    Ubiquitty::Terminal source;
    joinedTerminalFixture(source);
    const auto bytes = encodeHandoverTerminal(source, 91, terminalLimits);
    REQUIRE(bytes);
    auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->generation, 91u);
    auto& successor = *restored->terminal;
    CHECK_EQ(successor.richTextBlocks.nextSerial(), 731u);
    REQUIRE(successor.softFont && successor.pendingSound && successor.lineStore);
    CHECK(source.softFont.get() != successor.softFont.get());
    CHECK(source.pendingSound->front()->request.samplePCM != successor.pendingSound->front()->request.samplePCM);
    CHECK(source.images.stored[0].pixels != successor.images.stored[0].pixels);
    const auto roundTrip = encodeHandoverTerminal(successor, 91, terminalLimits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(bytes->view(), roundTrip->view()));
    for (auto* terminal : { &source, &successor }) {
        UbiquittyTest::feed(*terminal, "\xcc\x81\033[b\r\n\033]2;blocked\007\005"
                                       "\033]8;;https://example.test/new\033\\new\033]8;;\033\\"
                                       "\033[?1049halternate\033[?1049l");
        Ubiquitty::Image::advanceFrames(terminal, 150);
    }
    CHECK(replies(source) == replies(successor));
    const auto before = encodeHandoverTerminal(source, 92, terminalLimits);
    const auto after = encodeHandoverTerminal(successor, 92, terminalLimits);
    REQUIRE(before && after);
    const auto beforeSections = splitTerminalRecord(before->view());
    const auto afterSections = splitTerminalRecord(after->view());
    for (std::size_t index = 0; index < beforeSections.size(); ++index) {
        if (beforeSections[index] != afterSections[index]) {
            const auto mismatch = std::ranges::mismatch(beforeSections[index], afterSections[index]);
            std::fprintf(stderr, "joined section %zu mismatch at %zu; sizes %zu/%zu\n", index,
                static_cast<std::size_t>(mismatch.in1 - beforeSections[index].begin()), beforeSections[index].size(), afterSections[index].size());
        }
    }
    CHECK(std::ranges::equal(before->view(), after->view()));
}

TEST(compactScreensAndWrappedHistorySurviveCheckpointHandoverAndCancellation)
{
    using Ubiquitty::Column;
    using Ubiquitty::Row;
    using Ubiquitty::ScreenMode;
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        Ubiquitty::Terminal source;
        Ubiquitty::resize(source, 3, 8);
        // The full rows cycle through the ring before the checkpoint borrows either screen.
        UbiquittyTest::feed(source, "\033[3;1H\r\n01234567ABCDEFGHabcdefghIJKLMNOPQRSTUVWXqrstuvwx");
        REQUIRE(source.grid->compactASCIIRow(Row(0)));
        REQUIRE(source.grid->compactASCIIRow(Row(2)));
        REQUIRE(source.lineStore && source.lineStore->count() > 0);
        const auto* history = source.lineStore->at(source.lineStore->count() - 1);
        REQUIRE(history && history->wrapped);
        CHECK_EQ(history->length, 8);
        UbiquittyTest::feed(source, "\033[?1049h\033[H111111112222222233333333\033[3;1H\r\nALT");
        REQUIRE(source.grid->compactASCIIRow(Row(2)));
        if (screen == ScreenMode::Primary)
            UbiquittyTest::feed(source, "\033[?1049lQ");
        REQUIRE(source.grid->compactASCIIRow(Row(2)));

        auto bytes = encodeHandoverTerminal(source, 91, terminalLimits);
        REQUIRE(bytes);
        auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
        REQUIRE(restored);
        auto& successor = *restored->terminal;
        REQUIRE(successor.lineStore);
        CHECK_EQ(successor.lineStore->count(), source.lineStore->count());
        CHECK(UbiquittyTest::firstDisagreement(source, successor).empty());
        bytes->data.reset();

        for (auto* terminal : { &source, &successor })
            UbiquittyTest::feed(*terminal, "\033T");
        CHECK(UbiquittyTest::firstDisagreement(source, successor).empty());
        if (screen == ScreenMode::Primary) {
            CHECK_EQ(successor.grid->cellAt(Row(2), Column(7))->codePoint, std::uint32_t { 'x' });
            CHECK_EQ(successor.screens.active->cursorX, 7);
            CHECK(successor.screens.active->wrapPending);
        } else {
            CHECK_EQ(successor.grid->cellAt(Row(2), Column(2))->codePoint, 0u);
            CHECK_EQ(successor.screens.active->cursorX, 2);
        }
        for (auto* terminal : { &source, &successor })
            UbiquittyTest::feed(*terminal, "\r\nnext\033T\033[?1049lR\033T");
        CHECK(UbiquittyTest::firstDisagreement(source, successor).empty());
        const auto continuedSource = encodeHandoverTerminal(source, 92, terminalLimits);
        const auto continuedSuccessor = encodeHandoverTerminal(successor, 92, terminalLimits);
        REQUIRE(continuedSource && continuedSuccessor);
        CHECK(std::ranges::equal(continuedSource->view(), continuedSuccessor->view()));
    }
}

TEST(joinedTerminalHandoverAdmitsTheLastSectionBeforeConstructingAnyTerminal)
{
    Ubiquitty::Terminal source;
    joinedTerminalFixture(source);
    const auto bytes = encodeHandoverTerminal(source, 1, terminalLimits);
    REQUIRE(bytes);
    auto sections = splitTerminalRecord(bytes->view());
    sections[8][6] = 2; // Canonically invalid host Boolean, with both digests refreshed.
    refreshImageDigest(sections[8]);
    const auto invalid = joinTerminalRecord(sections, source.richTextBlocks.nextSerial());
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(decodeHandoverTerminal(invalid, terminalLimits).error() == HandoverBodyError::Malformed);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    auto small = terminalLimits;
    small.images.maxPixelBytes = 0;
    CHECK(decodeHandoverTerminal(bytes->view(), small).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    small = terminalLimits;
    small.maxScreenCellBytes = 1;
    CHECK(decodeHandoverTerminal(bytes->view(), small).error() == HandoverBodyError::OverCapacity);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(Ubiquitty::Terminal));
        CHECK(decodeHandoverTerminal(bytes->view(), terminalLimits).error() == HandoverBodyError::AllocationRefused);
    }
    REQUIRE(decodeHandoverTerminal(bytes->view(), terminalLimits));
}

TEST(joinedTerminalHandoverRejectsMixedEpochsAndDictionaryOwners)
{
    Ubiquitty::Terminal source;
    joinedTerminalFixture(source);
    const auto bytes = encodeHandoverTerminal(source, 2, terminalLimits);
    REQUIRE(bytes);
    auto sections = splitTerminalRecord(bytes->view());
    Ubiquitty::Terminal foreign;
    foreign.historyEpoch = 17;
    const auto history = encodeHandoverHistory(foreign, historyLimits);
    REQUIRE(history);
    sections[1].assign(history->view().begin(), history->view().end());
    CHECK(decodeHandoverTerminal(joinTerminalRecord(sections, 731), terminalLimits).error() == HandoverBodyError::Malformed);
    sections = splitTerminalRecord(bytes->view());
    const std::array<std::uint32_t, 2> foreignCluster { 'z', 0x301 };
    (void)foreign.clusterPool.intern(foreignCluster);
    const auto text = encodeHandoverTextStores(foreign, textLimits);
    REQUIRE(text);
    sections[3].assign(text->view().begin(), text->view().end());
    CHECK(decodeHandoverTerminal(joinTerminalRecord(sections, 731), terminalLimits).error() == HandoverBodyError::Malformed);
    sections = splitTerminalRecord(bytes->view());
    UbiquittyTest::feed(foreign, "\033]8;;https://example.test/foreign\033\\X");
    const auto links = encodeHandoverLinks(foreign, terminalLimits.maxLinkBytes);
    REQUIRE(links);
    sections[2].assign(links->view().begin(), links->view().end());
    CHECK(decodeHandoverTerminal(joinTerminalRecord(sections, 731), terminalLimits).error() == HandoverBodyError::Malformed);
}

TEST(joinedTerminalHandoverRejectsFramingAndGroundViolations)
{
    Ubiquitty::Terminal source;
    const auto bytes = encodeHandoverTerminal(source, 3, terminalLimits);
    REQUIRE(bytes);
    for (const auto length : { std::size_t(0), std::size_t(5), bytes->size / 2, bytes->size - 1 })
        CHECK(!decodeHandoverTerminal(bytes->view().first(length), terminalLimits));
    auto invalid = std::vector<std::uint8_t>(bytes->view().begin(), bytes->view().end());
    invalid.insert(invalid.end() - 16, 0);
    refreshImageDigest(invalid);
    CHECK(decodeHandoverTerminal(invalid, terminalLimits).error() == HandoverBodyError::Malformed);
    auto exact = terminalLimits;
    exact.maxRecordBytes = bytes->size;
    REQUIRE(encodeHandoverTerminal(source, 3, exact));
    --exact.maxRecordBytes;
    CHECK(encodeHandoverTerminal(source, 3, exact).error() == HandoverBodyError::OverCapacity);
    CHECK(decodeHandoverTerminal(bytes->view(), exact).error() == HandoverBodyError::OverCapacity);
    CHECK(!encodeHandoverTerminal(source, 0, terminalLimits));
    UbiquittyTest::feed(source, "\033[12;");
    CHECK(encodeHandoverTerminal(source, 3, terminalLimits).error() == HandoverBodyError::NotAtGround);
}

TEST(joinedTerminalHandoverKeepsSizedCellsAndRejectsAFalseBlankTail)
{
    Ubiquitty::Terminal source;
    source.drawsSizedText = true;
    source.drawsProportionalText = true;
    UbiquittyTest::feed(source, "\033]66;p=1:c=12;proportional text\007\r\nplain");
    const auto bytes = encodeHandoverTerminal(source, 4, terminalLimits);
    REQUIRE(bytes);
    auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->drawsSizedText);
    const auto after = encodeHandoverTerminal(*restored->terminal, 4, terminalLimits);
    REQUIRE(after);
    CHECK(std::ranges::equal(bytes->view(), after->view()));
    auto sections = splitTerminalRecord(bytes->view());
    // Width zero claims a blank first row although it contains the sized run.
    writeLittle(sections[9], 6, 0, 2);
    refreshImageDigest(sections[9]);
    CHECK(decodeHandoverTerminal(joinTerminalRecord(sections, source.richTextBlocks.nextSerial()), terminalLimits).error() == HandoverBodyError::Malformed);
    CHECK(!source.grid->restoreRowContentWidth(Ubiquitty::ScreenMode::Primary, Ubiquitty::Row(0), 0));
}

TEST(joinedTerminalHandoverKeepsIndependentRendererDeclarations)
{
    for (const bool drawsSized : { false, true }) {
        for (const bool drawsProportional : { false, true }) {
            Ubiquitty::Terminal source;
            source.drawsSizedText = drawsSized;
            source.drawsProportionalText = drawsProportional;
            UbiquittyTest::feed(source, "\033]66;p=1:c=6;Run\007\033[3;1H\033]66;s=2;X\007");
            const auto bytes = encodeHandoverTerminal(source, 4, terminalLimits);
            REQUIRE(bytes);
            auto restored = decodeHandoverTerminal(bytes->view(), terminalLimits);
            REQUIRE(restored);
            CHECK(restored->terminal->drawsSizedText == drawsSized);
            CHECK(restored->terminal->drawsProportionalText == drawsProportional);
            CHECK(screenCheckpoint(*restored->terminal) == screenCheckpoint(source));
            UbiquittyTest::feed(*restored->terminal, "\033]66;p=1:q=1:i=3;OK\007");
            char reply[128];
            const auto length = UbiquittyTest::drain(*restored->terminal, reply);
            CHECK(std::string_view(reply, static_cast<std::size_t>(length))
                == (drawsProportional ? "\033]66;i=3:c=2\007" : ""));
        }
    }
}

TEST(panePublicationHandoverPreservesDictionaryHistoryAndImageRevisions)
{
    Ubiquitty::Terminal terminal;
    joinedTerminalFixture(terminal);
    const ID128 pane { 17, 18 };
    PaneSource source(terminal, pane);
    source.publish(FrameClass::Settled, 100);
    REQUIRE(terminal.images.placements[0].used);
    ++terminal.images.placements[0].cursorX;
    source.publish(FrameClass::Settled, 200);
    REQUIRE(source.images().state().revision > 1);
    const HandoverPublicationLimits publicationLimits { 1024 * 1024, 65536, 4096, 4096, 4096 };
    auto bytes = encodeHandoverPublication(source, publicationLimits);
    REQUIRE(bytes);
    const auto terminalBytes = encodeHandoverTerminal(terminal, source.generation(), terminalLimits);
    REQUIRE(terminalBytes);
    auto successor = decodeHandoverTerminal(terminalBytes->view(), terminalLimits);
    auto publication = decodeHandoverPublication(bytes->view(), publicationLimits);
    REQUIRE(successor && publication);
    PaneSource restored(*successor->terminal, pane);
    REQUIRE(restored.restorePublication(std::move(*publication)));
    CHECK_EQ(restored.images().state().revision, source.images().state().revision);
    auto roundTrip = encodeHandoverPublication(restored, publicationLimits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(bytes->view(), roundTrip->view()));
    UbiquittyTest::feed(terminal, "\r\nnext line\033[31mred\033[0m\r\n");
    UbiquittyTest::feed(*successor->terminal, "\r\nnext line\033[31mred\033[0m\r\n");
    source.publish(FrameClass::Settled, 400);
    restored.publish(FrameClass::Settled, 400);
    bytes = encodeHandoverPublication(source, publicationLimits);
    roundTrip = encodeHandoverPublication(restored, publicationLimits);
    REQUIRE(bytes && roundTrip);
    CHECK(std::ranges::equal(bytes->view(), roundTrip->view()));
    auto limited = publicationLimits;
    limited.maxPens = 0;
    CHECK(decodeHandoverPublication(bytes->view(), limited).error() == HandoverBodyError::OverCapacity);
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!decodeHandoverPublication(bytes->view().first(length), publicationLimits));
    auto mismatching = decodeHandoverPublication(bytes->view(), publicationLimits);
    REQUIRE(mismatching);
    ++mismatching->images.placements[0].geometry.cursorX;
    PaneSource refused(*successor->terminal, pane);
    CHECK(!refused.restorePublication(std::move(*mismatching)));
    CHECK_EQ(refused.generation(), 0u);
    CHECK_EQ(refused.images().state().revision, 0u);
}

// A host reads only the state format it writes. A publication of an earlier
// version is refused, so its block hashes are never taken as this build's.
TEST(anEarlierPublicationVersionIsRefused)
{
    for (const bool hasRichText : { false, true }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 6, 40);
        UbiquittyTest::feed(terminal, hasRichText ? "\033]7404;w=10:h=1:C=1:i=1:D=9;{\\rtf1 source}\033\\" : "ordinary text");
        const ID128 pane { 17, 18 };
        PaneSource source(terminal, pane);
        source.publish(FrameClass::Settled, 100);
        const auto bytes = encodeHandoverPublication(source, paneLimits.publication);
        REQUIRE(bytes);
        std::vector<std::uint8_t> earlier(bytes->view().begin(), bytes->view().end());
        writeLittle(earlier, 0, oldestHandoverBodyVersion - 1, 2);
        refreshImageDigest(earlier);
        const auto refused = decodeHandoverPublication(earlier, paneLimits.publication);
        REQUIRE(!refused);
        CHECK(refused.error() == HandoverBodyError::Malformed);
    }
}

TEST(portableSessionFactsCarryClocksOriginsAndOwnedStringsWithoutAuthority)
{
    SessionFacts facts;
    facts.sessionID = 17;
    facts.createdUnix = 100;
    facts.outputOrigin = Ubiquitty::CapabilityOrigin::Remote;
    facts.driverOriginFloor = Ubiquitty::CapabilityOrigin::Agent;
    facts.firstInputPrincipal = "token:first-driver";
    facts.hasMultipleInputPrincipals = true;
    facts.lastInputClientID = 20;
    facts.lastHumanInputClientID = 21;
    facts.stampedTypedInputWrites = 37;
    facts.seenPersonFocusGains = 12;
    facts.recordedActivityGeneration = 0;
    facts.pendingGeometry = PaneGeometry { 30, 81 };
    facts.presentedApplied = SessionFacts::Presented { { 41, 42 }, 99, 8000 };
    facts.presentedDueAt = 9000;
    facts.presentedStandIn = ID128 { 51, 52 };
    facts.publishedEchoFacts = PaneEchoFacts { .isSecureEntry = true, .isAtPrompt = false, .interruptCharacters = { .interrupt = 1, .quit = {}, .suspend = 26 }, .foregroundGeneration = 13 };
    facts.paneFacts.currentDirectory = "/task/owned";
    facts.paneFacts.processChain = "editor - shell";
    facts.reportedProgramStatus.push_back({ .id = "test", .app = "", .title = "Working", .message = "", .state = Ubiquitty::OSC::ProgramState::Working });
    facts.observedDirectory = facts.paneFacts.currentDirectory;
    facts.landedPastePaths = { "/task/owned/paste-a", "/task/owned/paste-b" };
    HandoverPaneLimits receiver;
    receiver.maxSessionBytes = 64 * 1024;
    receiver.maxLandedPastePaths = 4;
    const auto bytes = encodeHandoverSessionFacts(facts, receiver);
    REQUIRE(bytes);
    auto restored = decodeHandoverSessionFacts(bytes->view(), receiver);
    REQUIRE(restored);
    CHECK(restored->outputOrigin == facts.outputOrigin);
    CHECK(restored->driverOriginFloor == facts.driverOriginFloor);
    CHECK(restored->firstInputPrincipal == facts.firstInputPrincipal);
    CHECK(restored->hasMultipleInputPrincipals == facts.hasMultipleInputPrincipals);
    CHECK(restored->recordedActivityGeneration.has_value());
    CHECK(restored->publishedEchoFacts == facts.publishedEchoFacts);
    CHECK(restored->landedPastePaths == facts.landedPastePaths);
    const auto encodedAgain = encodeHandoverSessionFacts(*restored, receiver);
    REQUIRE(encodedAgain);
    CHECK(std::ranges::equal(bytes->view(), encodedAgain->view()));
    for (std::size_t length = 0; length < bytes->size; ++length)
        CHECK(!decodeHandoverSessionFacts(bytes->view().first(length), receiver));
    receiver.maxLandedPastePaths = 1;
    CHECK(!decodeHandoverSessionFacts(bytes->view(), receiver));
    receiver.maxLandedPastePaths = 4;
    facts.landedPastePaths[0] = std::string("/task/a\0b", 9);
    CHECK(!encodeHandoverSessionFacts(facts, receiver));
}

TEST(sessionInputPrincipalHistoryDistinguishesFreshAndInheritedUnknownSessions)
{
    SessionFacts fresh;
    fresh.sessionID = 1;
    CHECK(!fresh.firstInputPrincipal);
    CHECK(!fresh.hasMultipleInputPrincipals);
    HandoverPaneLimits limits;
    limits.maxSessionBytes = 64 * 1024;
    const auto encoded = encodeHandoverSessionFacts(fresh, limits);
    REQUIRE(encoded);
    const auto restored = decodeHandoverSessionFacts(encoded->view(), limits);
    REQUIRE(restored);
    CHECK(!restored->firstInputPrincipal);
    CHECK(!restored->hasMultipleInputPrincipals);

    auto unattributed = fresh;
    unattributed.firstInputPrincipal = "";
    CHECK(!encodeHandoverSessionFacts(unattributed, limits));
    unattributed.hasMultipleInputPrincipals = true;
    const auto unattributedBytes = encodeHandoverSessionFacts(unattributed, limits);
    REQUIRE(unattributedBytes);
    const auto remembered = decodeHandoverSessionFacts(unattributedBytes->view(), limits);
    REQUIRE(remembered && remembered->firstInputPrincipal);
    CHECK(remembered->firstInputPrincipal->empty());
    CHECK(remembered->hasMultipleInputPrincipals);
    std::vector<std::uint8_t> falselyAttributed(unattributedBytes->view().begin(), unattributedBytes->view().end());
    falselyAttributed[falselyAttributed.size() - 17] = 0;
    refreshImageDigest(falselyAttributed);
    CHECK(!decodeHandoverSessionFacts(falselyAttributed, limits));

    // Facts of an earlier version are refused rather than read without the
    // input history this version carries.
    std::vector<std::uint8_t> earlier(encoded->view().begin(), encoded->view().end());
    writeLittle(earlier, 0, oldestHandoverBodyVersion - 1, 2);
    refreshImageDigest(earlier);
    const auto refused = decodeHandoverSessionFacts(earlier, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::Malformed);
}

TEST(sessionInputPrincipalHistoryRejectsMalformedTagsAndOversizedOrNULPrincipals)
{
    SessionFacts facts;
    facts.sessionID = 1;
    facts.firstInputPrincipal = "token:first-driver";
    HandoverPaneLimits limits;
    limits.maxSessionBytes = 64 * 1024;
    auto encoded = encodeHandoverSessionFacts(facts, limits);
    REQUIRE(encoded);
    const auto trailerBytes = 1 + sizeof(std::uint32_t) + facts.firstInputPrincipal->size() + 1;
    REQUIRE(encoded->size >= trailerBytes + 16);
    const auto trailer = encoded->size - trailerBytes - 16;
    for (const auto offset : { trailer, encoded->size - 17 }) {
        std::vector<std::uint8_t> changed(encoded->view().begin(), encoded->view().end());
        changed[offset] = 2;
        refreshImageDigest(changed);
        CHECK(!decodeHandoverSessionFacts(changed, limits));
    }
    std::vector<std::uint8_t> nul(encoded->view().begin(), encoded->view().end());
    nul[trailer + 1 + sizeof(std::uint32_t)] = 0;
    refreshImageDigest(nul);
    CHECK(!decodeHandoverSessionFacts(nul, limits));
    facts.firstInputPrincipal = std::string("token:first\0driver", 18);
    CHECK(!encodeHandoverSessionFacts(facts, limits));

    facts.firstInputPrincipal = std::string(4096, 'p');
    encoded = encodeHandoverSessionFacts(facts, limits);
    REQUIRE(encoded);
    REQUIRE(decodeHandoverSessionFacts(encoded->view(), limits));
    std::vector<std::uint8_t> overlong(encoded->view().begin(), encoded->view().end());
    const auto lengthOffset = overlong.size() - 16 - 1 - 4096 - sizeof(std::uint32_t);
    writeLittle(overlong, lengthOffset, 4097, sizeof(std::uint32_t));
    overlong.insert(overlong.end() - 17, 'p');
    refreshImageDigest(overlong);
    CHECK(!decodeHandoverSessionFacts(overlong, limits));
    facts.firstInputPrincipal->push_back('p');
    CHECK(!encodeHandoverSessionFacts(facts, limits));
}

TEST(joinedPaneHandoverKeepsReceiptsEffectsGeometryAndAcceptedPTYOwnership)
{
    Ubiquitty::Terminal terminal;
    joinedTerminalFixture(terminal);
    const ID128 pane { 17, 18 }, client { 21, 22 }, attachment { 31, 32 };
    PaneSource source(terminal, pane, { 100, 50, maxPendingInputBytes });
    HandoverInputWriter predecessorWriter;
    source.setInputWriter(predecessorWriter);
    source.geometry().noteViewport(attachment, GeometryStanding::Driver, { 30, 90 }, 50);
    source.geometry().pin({ 30, 90 }, attachment);
    source.noteDriverFocus(attachment, Ubiquitty::Modes::FocusState::Focused, ClientKind::Human);
    REQUIRE(source.queueInput(client, 1, RawInput { { 'a' } }, 100, attachment) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 2, RawInput { { 'b' } }, 250, attachment) == EnqueueOutcome::Queued);
    source.noteReadWouldBlock(300);
    PaneFacts facts;
    facts.programTitle = "continued pane";
    facts.currentDirectory = "/task/owned";
    facts.rows = terminal.rows;
    facts.columns = terminal.columns;
    source.setFacts(facts);
    StatusRecords status;
    status.store.records.push_back({ .id = "build", .app = "", .title = "Working", .message = "", .state = Ubiquitty::OSC::ProgramState::Working });
    source.setStatus(status);
    source.publish(FrameClass::Settled, 300);
    REQUIRE(source.effects().addReader(attachment, client, 0));
    REQUIRE(source.appendEffect(NotificationEffect { "title", "body", "identity", Ubiquitty::NotificationOccasion::Always }, client));
    const std::array<std::uint8_t, 5> clipboardBytes { 0, 1, 2, 3, 255 };
    EffectClipboardBundle clipboard;
    clipboard.items[0] = { "application/octet-stream", clipboardBytes };
    clipboard.count = 1;
    REQUIRE(source.appendClipboardWrite(Ubiquitty::ClipboardBuffer::Clipboard, clipboard, client));
    REQUIRE(source.effects().noteSent(attachment, 2));
    REQUIRE(source.effects().noteState(attachment, AttachmentState::Lost, 350));
    SessionFacts session;
    session.sessionID = 17;
    session.createdUnix = 50;
    session.outputOrigin = Ubiquitty::CapabilityOrigin::Remote;
    session.describedStructuralGeneration = source.structuralGeneration();
    session.landedPastePaths = { "/task/owned/temporary" };
    const std::array<char, 9> accepted { 'a', 'b', '\033', '[', '2', '0', '1', '~', 'r' };
    const std::array<Ubiquitty::PAL::OutputQueue::Run, 3> runs { { { 7, 2 }, { 8, 6 }, { 0, 1 } } };
    const auto pending = Ubiquitty::PAL::OutputQueue::Unwritten::copyOf(accepted, runs);
    REQUIRE(pending);
    const auto bytes = encodePaneHandover(source, session, *pending, paneLimits);
    REQUIRE(bytes);
    auto restored = decodePaneHandover(bytes->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->source->paneID() == pane);
    CHECK(restored->source->facts() == source.facts());
    CHECK(restored->source->status() == source.status());
    CHECK_EQ(restored->source->inputLedger().lastWritten(client).value_or(0), 2u);
    CHECK_EQ(restored->source->settledAtPublish(client), 1u);
    CHECK(restored->source->geometry().status() == source.geometry().status());
    CHECK_EQ(restored->source->personFocusGains(), source.personFocusGains());
    REQUIRE(restored->unwrittenPTY.runs().size() == runs.size());
    CHECK(std::ranges::equal(restored->unwrittenPTY.bytes(), accepted));
    CHECK(restored->unwrittenPTY.bytes().data() != pending->bytes().data());
    for (std::size_t index = 0; index < runs.size(); ++index) {
        CHECK_EQ(restored->unwrittenPTY.runs()[index].owner, runs[index].owner);
        CHECK_EQ(restored->unwrittenPTY.runs()[index].bytes, runs[index].bytes);
    }
    auto repeated = encodePaneHandover(*restored->source, restored->session, restored->unwrittenPTY, paneLimits);
    REQUIRE(repeated);
    CHECK(std::ranges::equal(bytes->view(), repeated->view()));
    const auto effects = restored->source->effects().readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(effects && effects->size == 2);
    CHECK(effects->records[1].primary == client);
    const auto clipboardBody = decodeEffectBody(effects->records[1].kind, effects->records[1].payload);
    REQUIRE(clipboardBody);
    const auto bundle = decodeEffectClipboard(effects->records[1].data, maxEffectClipboardWriteBytes);
    REQUIRE(bundle && bundle->count == 1);
    CHECK(std::ranges::equal(bundle->items[0].data, clipboardBytes));
    HandoverInputWriter successorWriter;
    restored->source->setInputWriter(successorWriter);
    CHECK(restored->source->queueInput(client, 2, RawInput { { 'b' } }, 400, attachment) == EnqueueOutcome::Duplicate);
    CHECK(successorWriter.bytes.empty());
    for (auto* current : { &source, restored->source.get() }) {
        REQUIRE(current->queueInput(client, 3, RawInput { { 'c' } }, 450, attachment) == EnqueueOutcome::Queued);
        current->noteReadWouldBlock(600);
        REQUIRE(current->effects().resumeReader(attachment, client, 0));
        REQUIRE(current->effects().acknowledge(attachment, 2));
        current->effects().trim(600);
    }
    UbiquittyTest::feed(terminal, "\r\nfuture bytes\r\n");
    UbiquittyTest::feed(*restored->terminal, "\r\nfuture bytes\r\n");
    source.publish(FrameClass::Settled, 600);
    restored->source->publish(FrameClass::Settled, 600);
    const auto before = encodePaneHandover(source, session, *pending, paneLimits);
    const auto after = encodePaneHandover(*restored->source, restored->session, restored->unwrittenPTY, paneLimits);
    REQUIRE(before && after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    CHECK_EQ(successorWriter.bytes.size(), 1u);
    CHECK_EQ(successorWriter.bytes[0], 'c');
}

TEST(joinedPaneHandoverRefusesMixedOwnersAndBoundsWithoutDiscardingInput)
{
    Ubiquitty::Terminal terminal;
    UbiquittyTest::feed(terminal, "exact pane");
    PaneSource source(terminal, { 17, 18 });
    source.publish(FrameClass::Settled, 100);
    SessionFacts session;
    session.sessionID = 17;
    Ubiquitty::PAL::OutputQueue::Unwritten empty;
    const auto bytes = encodePaneHandover(source, session, empty, paneLimits);
    REQUIRE(bytes);
    for (std::size_t length = 0; length < bytes->size; length += 31)
        CHECK(!decodePaneHandover(bytes->view().first(length), paneLimits));
    CHECK(!decodePaneHandover(bytes->view().first(bytes->size - 1), paneLimits));
    auto exact = paneLimits;
    exact.maxRecordBytes = bytes->size;
    REQUIRE(decodePaneHandover(bytes->view(), exact));
    --exact.maxRecordBytes;
    CHECK(decodePaneHandover(bytes->view(), exact).error() == HandoverBodyError::OverCapacity);
    exact = paneLimits;
    exact.maxEffectRetainedBytes = 1;
    CHECK(!decodePaneHandover(bytes->view(), exact));
    std::vector<std::uint8_t> changed(bytes->view().begin(), bytes->view().end());
    writeLittle(changed, 2, 99, 8);
    refreshImageDigest(changed);
    CHECK(decodePaneHandover(changed, paneLimits).error() == HandoverBodyError::Malformed);
    session.sessionID = 99;
    CHECK(!encodePaneHandover(source, session, empty, paneLimits));
    session.sessionID = 17;
    REQUIRE(source.queueInput({ 21, 22 }, 1, RawInput { { 'a' } }, 100) == EnqueueOutcome::Queued);
    REQUIRE(encodePaneHandover(source, session, empty, paneLimits));
    CHECK(!source.inputLedger().lastWritten({ 21, 22 }));
}

TEST(joinedPaneHandoverRejectsTypedEffectAndPTYPartitionForgery)
{
    Ubiquitty::Terminal terminal;
    PaneSource source(terminal, { 17, 18 });
    source.publish(FrameClass::Settled, 100);
    SessionFacts session;
    session.sessionID = 17;
    const std::array<char, 3> payload { 'a', 'b', 'c' };
    const std::array<Ubiquitty::PAL::OutputQueue::Run, 1> runs { { { 9, 3 } } };
    const auto pending = Ubiquitty::PAL::OutputQueue::Unwritten::copyOf(payload, runs);
    REQUIRE(pending);
    const auto bytes = encodePaneHandover(source, session, *pending, paneLimits);
    REQUIRE(bytes);
    std::vector<std::uint8_t> changed(bytes->view().begin(), bytes->view().end());
    WireReader reader(changed);
    (void)reader.take(18);
    for (int index = 0; index < 4; ++index) {
        const auto length = reader.u32();
        (void)reader.take(length);
    }
    const auto sectionBytes = reader.u32();
    const auto offset = reader.offset();
    REQUIRE(reader.ok() && sectionBytes >= 22);
    // A zero run cannot claim the three admitted payload bytes.
    writeLittle(changed, offset + 12, 0, 4);
    const auto digest = digestOf(std::span(changed).subspan(offset, sectionBytes - 16));
    writeLittle(changed, offset + sectionBytes - 16, digest.low, 8);
    writeLittle(changed, offset + sectionBytes - 8, digest.high, 8);
    refreshImageDigest(changed);
    CHECK(decodePaneHandover(changed, paneLimits).error() == HandoverBodyError::Malformed);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(sizeof(PaneSource));
        CHECK(!decodePaneHandover(bytes->view(), paneLimits));
    }
    REQUIRE(decodePaneHandover(bytes->view(), paneLimits));
    const std::array<std::uint8_t, 1> malformedBell { 1 };
    REQUIRE(source.effects().append({ .generation = source.generation(), .kind = EffectKind::Bell, .payload = malformedBell }));
    CHECK(encodePaneHandover(source, session, *pending, paneLimits).error() == HandoverBodyError::Malformed);
    CHECK_EQ(source.effects().size(), 1u);
}

TEST(joinedPaneHandoverContinuesPartialPasteAndQueuedNeighbourWithoutReplay)
{
    struct Writer final : InputWriter {
        bool canWrite = true;
        std::vector<std::uint8_t> bytes;
        std::vector<ID128> authorities;
        WriteStatus write(ID128, const InputIntent& intent) override
        {
            if (!canWrite)
                return WriteStatus::Refused;
            if (const auto* raw = std::get_if<RawInput>(&intent))
                bytes.insert(bytes.end(), raw->bytes.begin(), raw->bytes.end());
            if (const auto* paste = std::get_if<PasteInput>(&intent))
                bytes.insert(bytes.end(), paste->text.begin(), paste->text.end());
            return WriteStatus::Written;
        }
        WriteStatus writeOwned(ID128 authority, ID128 client, const InputIntent& intent) override
        {
            const auto result = write(client, intent);
            if (result == WriteStatus::Written)
                authorities.push_back(authority);
            return result;
        }
    } before, after;
    Ubiquitty::Terminal terminal;
    const ID128 client { 21, 22 }, neighbour { 31, 32 }, owner { 41, 42 }, otherOwner { 51, 52 };
    PaneSource source(terminal, { 17, 18 });
    source.setInputWriter(before);
    REQUIRE(source.queueInput(client, 1, PasteInput { { 'a', 'b' }, 0, PasteEnd::More }, 100, owner) == EnqueueOutcome::Queued);
    before.canWrite = false;
    REQUIRE(source.queueInput(client, 1, PasteInput { { 'c', 'd' }, 2, PasteEnd::More }, 110, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 2, RawInput { { 'X' } }, 120, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(neighbour, 1, RawInput { { 'N' } }, 130, otherOwner) == EnqueueOutcome::Queued);
    source.publish(FrameClass::Settled, 140);
    SessionFacts session;
    session.sessionID = 17;
    Ubiquitty::PAL::OutputQueue::Unwritten pending;
    const auto bytes = encodePaneHandover(source, session, pending, paneLimits);
    REQUIRE(bytes);
    auto restored = decodePaneHandover(bytes->view(), paneLimits);
    REQUIRE(restored);
    restored->source->setInputWriter(after);
    before.bytes.clear();
    before.authorities.clear();
    before.canWrite = true;
    for (auto* current : { &source, restored->source.get() }) {
        current->drainInput(200);
        REQUIRE(current->queueInput(client, 1, PasteInput { { 'a', 'b', 'c', 'd' }, 0, PasteEnd::More }, 210, owner) == EnqueueOutcome::Duplicate);
        REQUIRE(current->queueInput(client, 1, PasteInput { { 'e', 'f' }, 4, PasteEnd::Final }, 220, owner) == EnqueueOutcome::Queued);
        current->publish(FrameClass::Settled, 230);
    }
    CHECK(before.bytes == after.bytes);
    CHECK(before.authorities == after.authorities);
    CHECK(std::ranges::equal(after.bytes, std::array<std::uint8_t, 6> { 'c', 'd', 'N', 'e', 'f', 'X' }));
    CHECK_EQ(restored->source->inputLedger().lastWritten(client).value_or(0), 2u);
    CHECK_EQ(restored->source->inputLedger().lastWritten(neighbour).value_or(0), 1u);
    const auto original = encodePaneHandover(source, session, pending, paneLimits);
    const auto continued = encodePaneHandover(*restored->source, session, pending, paneLimits);
    REQUIRE(original && continued);
    CHECK(std::ranges::equal(original->view(), continued->view()));
}

TEST(joinedPaneHandoverKeepsAnOvertakingSignalsOriginalWriteTime)
{
    struct Writer final : InputWriter {
        std::vector<std::uint8_t> bytes;
        SignalEffect signalEffect(const InputIntent& intent) override
        {
            const auto* raw = std::get_if<RawInput>(&intent);
            return raw && raw->bytes == std::vector<std::uint8_t> { 3 } ? SignalEffect::Signals : SignalEffect::None;
        }
        WriteStatus write(ID128, const InputIntent& intent) override
        {
            if (const auto* raw = std::get_if<RawInput>(&intent))
                bytes.insert(bytes.end(), raw->bytes.begin(), raw->bytes.end());
            if (const auto* paste = std::get_if<PasteInput>(&intent))
                bytes.insert(bytes.end(), paste->text.begin(), paste->text.end());
            return WriteStatus::Written;
        }
    } before, after;
    Ubiquitty::Terminal terminal;
    const ID128 client { 21, 22 }, owner { 31, 32 };
    PaneSource source(terminal, { 17, 18 }, { 100, 50, maxPendingInputBytes });
    source.setInputWriter(before);
    REQUIRE(source.queueInput(client, 1, PasteInput { { 'a' }, 0, PasteEnd::More }, 100, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 2, RawInput { { 'X' } }, 200, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 3, RawInput { { 3 } }, 300, owner) == EnqueueOutcome::Queued);
    source.publish(FrameClass::Settled, 350);
    SessionFacts session;
    session.sessionID = 17;
    Ubiquitty::PAL::OutputQueue::Unwritten pending;
    const auto encoded = encodePaneHandover(source, session, pending, paneLimits);
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    restored->source->setInputWriter(after);
    before.bytes.clear();
    for (auto* current : { &source, restored->source.get() }) {
        REQUIRE(current->queueInput(client, 1, PasteInput { { 'b' }, 1, PasteEnd::Final }, 500, owner) == EnqueueOutcome::Queued);
        CHECK_EQ(current->inputLedger().lastWritten(client).value_or(0), 3u);
        CHECK_EQ(current->inputLedger().lastWrittenAt(client).value_or(0), 300u);
        current->publish(FrameClass::Settled, 510);
    }
    CHECK(before.bytes == after.bytes);
    CHECK(std::ranges::equal(after.bytes, std::array<std::uint8_t, 2> { 'b', 'X' }));
    const auto again = encodePaneHandover(*restored->source, session, pending, paneLimits);
    REQUIRE(again);
    auto next = decodePaneHandover(again->view(), paneLimits);
    REQUIRE(next);
    next->source->noteReadWouldBlock(599);
    CHECK_EQ(next->source->inputLedger().settledThrough(client), 0u);
    next->source->noteReadWouldBlock(600);
    CHECK_EQ(next->source->inputLedger().settledThrough(client), 3u);
}

TEST(queuedInputContinuationValidatesPartitionsBeforeConsumingTheCandidate)
{
    InputLedger ledger;
    PaneInputQueue source;
    const ID128 client { 21, 22 }, owner { 31, 32 };
    REQUIRE(source.enqueue(ledger, client, 1, PasteInput { { 'a', 'b' }, 0, PasteEnd::More }, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueue(ledger, client, 2, RawInput { { 'x' } }, owner) == EnqueueOutcome::Queued);
    const auto snapshot = source.continuation(ledger);
    REQUIRE(snapshot);
    PaneInputQueue destination;
    auto invalid = *snapshot;
    ++invalid.heldBytes;
    CHECK(!destination.restoreContinuation(std::move(invalid), ledger));
    CHECK_EQ(invalid.pending.size(), 2u);
    invalid = *snapshot;
    ++std::get<PasteInput>(invalid.pending[0].intent).offset;
    CHECK(!destination.restoreContinuation(std::move(invalid), ledger));
    invalid = *snapshot;
    invalid.pending[0].authority = { 99, 100 };
    CHECK(!destination.restoreContinuation(std::move(invalid), ledger));
    invalid = *snapshot;
    invalid.pending.push_back(invalid.pending[1]);
    ++invalid.heldBytes;
    CHECK(!destination.restoreContinuation(std::move(invalid), ledger));
    invalid = *snapshot;
    invalid.pastes[0].hasEnded = true;
    CHECK(!destination.restoreContinuation(std::move(invalid), ledger));
    CHECK(destination.isEmpty());
    CHECK_EQ(destination.heldBytes(), 0u);
    auto accepted = *snapshot;
    REQUIRE(destination.restoreContinuation(std::move(accepted), ledger));
    CHECK(accepted.pending.empty());
    CHECK_EQ(destination.heldBytes(), source.heldBytes());
    REQUIRE(destination.abortPaste(ledger, client, 1, owner));
    const auto aborted = destination.continuation(ledger);
    REQUIRE(aborted);
    PaneInputQueue another;
    auto owned = *aborted;
    REQUIRE(another.restoreContinuation(std::move(owned), ledger));
    CHECK_EQ(another.heldBytes(), destination.heldBytes());
}

TEST(joinedPaneHandoverCarriesEveryUnencodedInputKindAndOwner)
{
    Ubiquitty::Terminal terminal;
    PaneSource source(terminal, { 17, 18 });
    const ID128 client { 21, 22 }, owner { 31, 32 };
    const KeyEvent key { .key = 41, .modifiers = 7, .kind = KeyEventKind::Repeat, .text = "x", .unshiftedCodePoint = 'x', .shiftedCodePoint = 'X', .baseLayoutCodePoint = 'q', .wasModified = true };
    const PointerEvent pointer { .kind = PointerEventKind::Motion, .button = 65, .modifiers = 3, .row = 12, .column = 34, .pixelX = 123, .pixelY = 456 };
    REQUIRE(source.queueInput(client, 1, RawInput { { 0, 128, 255 } }, 100, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 2, KeyInput { { key } }, 110, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 3, PointerInput { { pointer } }, 120, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.queueInput(client, 4, FocusInput { Ubiquitty::Modes::FocusState::Focused }, 130, owner) == EnqueueOutcome::Queued);
    source.publish(FrameClass::Settled, 140);
    SessionFacts session;
    session.sessionID = 17;
    Ubiquitty::PAL::OutputQueue::Unwritten pending;
    const auto bytes = encodePaneHandover(source, session, pending, paneLimits);
    REQUIRE(bytes);
    const auto restored = decodePaneHandover(bytes->view(), paneLimits);
    REQUIRE(restored);
    const auto state = restored->source->continuation();
    REQUIRE(state && state->queue.pending.size() == 4);
    for (const auto& held : state->queue.pending) {
        CHECK(held.clientInstance == client);
        CHECK(held.authority == owner);
    }
    CHECK(std::get<RawInput>(state->queue.pending[0].intent).bytes == (std::vector<std::uint8_t> { 0, 128, 255 }));
    CHECK(std::get<KeyInput>(state->queue.pending[1].intent).events == std::vector<KeyEvent> { key });
    CHECK(std::get<PointerInput>(state->queue.pending[2].intent).events == std::vector<PointerEvent> { pointer });
    CHECK(std::get<FocusInput>(state->queue.pending[3].intent).focus == Ubiquitty::Modes::FocusState::Focused);
    const auto roundTrip = encodePaneHandover(*restored->source, session, pending, paneLimits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(bytes->view(), roundTrip->view()));
    auto malformed = *state;
    std::get<KeyInput>(malformed.queue.pending[1].intent).events[0].kind = static_cast<KeyEventKind>(99);
    CHECK(!PaneSource::restored(terminal, std::move(malformed)));
    malformed = *state;
    malformed.queue.pending.resize(PaneInputQueue::maxContinuationIntents + 1);
    CHECK(!PaneSource::restored(terminal, std::move(malformed)));
}

TEST(queuedPasteContinuationMayArriveAfterTheKeysItKeepsWaiting)
{
    struct Writer final : InputWriter {
        std::vector<std::uint8_t> bytes;
        WriteStatus write(ID128, const InputIntent& intent) override
        {
            if (const auto* raw = std::get_if<RawInput>(&intent))
                bytes.insert(bytes.end(), raw->bytes.begin(), raw->bytes.end());
            if (const auto* paste = std::get_if<PasteInput>(&intent))
                bytes.insert(bytes.end(), paste->text.begin(), paste->text.end());
            return WriteStatus::Written;
        }
    } before, after;
    InputLedger originalLedger, restoredLedger;
    PaneInputQueue source, restored;
    const ID128 client { 21, 22 }, owner { 31, 32 };
    REQUIRE(source.enqueue(originalLedger, client, 1, PasteInput { { 'a' }, 0, PasteEnd::More }, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueue(originalLedger, client, 2, RawInput { { 'X' } }, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueue(originalLedger, client, 1, PasteInput { { 'b' }, 1, PasteEnd::Final }, owner) == EnqueueOutcome::Queued);
    auto snapshot = source.continuation(originalLedger);
    REQUIRE(snapshot);
    REQUIRE(restored.restoreContinuation(std::move(*snapshot), restoredLedger));
    REQUIRE(source.drain(before, originalLedger, 100));
    REQUIRE(restored.drain(after, restoredLedger, 100));
    CHECK(before.bytes == after.bytes);
    CHECK(std::ranges::equal(after.bytes, std::array<std::uint8_t, 3> { 'a', 'b', 'X' }));
    CHECK_EQ(restoredLedger.lastWritten(client).value_or(0), 2u);
    CHECK(restored.isEmpty());
}

TEST(queuedSignalRetainsEveryEndedPasteAndUnnumberedSystemInput)
{
    struct Writer final : InputWriter {
        std::vector<ID128> ended;
        std::vector<std::uint8_t> bytes;
        WriteStatus write(ID128, const InputIntent& intent) override
        {
            if (const auto* raw = std::get_if<RawInput>(&intent))
                bytes.insert(bytes.end(), raw->bytes.begin(), raw->bytes.end());
            return WriteStatus::Written;
        }
        WriteStatus writeSignalOwned(ID128, ID128 client, const InputIntent& intent, std::span<const ID128> pastes) override
        {
            ended.assign(pastes.begin(), pastes.end());
            return write(client, intent);
        }
    } before, after;
    InputLedger originalLedger, restoredLedger;
    PaneInputQueue source, restored;
    const ID128 client { 21, 22 }, owner { 31, 32 };
    REQUIRE(source.enqueue(originalLedger, client, 1, PasteInput { { 'a' }, 0, PasteEnd::More }, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueue(originalLedger, client, 2, PasteInput { { 'b' }, 0, PasteEnd::More }, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueueSignal(originalLedger, client, 3, RawInput { { 3 } }, SignalEffect::SignalsAndDiscards, 50, owner) == EnqueueOutcome::Queued);
    REQUIRE(source.enqueueUnnumbered(RawInput { { 'R' } }) == EnqueueOutcome::Queued);
    const auto ledger = originalLedger.continuation();
    REQUIRE(ledger && restoredLedger.restoreContinuation(*ledger));
    auto snapshot = source.continuation(originalLedger);
    REQUIRE(snapshot);
    REQUIRE(snapshot->pending[0].endedPastes == (std::vector<ID128> { client, client }));
    REQUIRE(restored.restoreContinuation(std::move(*snapshot), restoredLedger));
    REQUIRE(source.drain(before, originalLedger, 100));
    REQUIRE(restored.drain(after, restoredLedger, 100));
    CHECK(before.ended == after.ended);
    CHECK(after.ended == (std::vector<ID128> { client, client }));
    CHECK(before.bytes == after.bytes);
    CHECK(std::ranges::equal(after.bytes, std::array<std::uint8_t, 2> { 3, 'R' }));
    CHECK_EQ(restoredLedger.lastWritten(client).value_or(0), 3u);
}

namespace {

/// The frozen pane record of the version this build writes, by the content
/// hash the fuzzer's corpus names its units with. A grammar change takes a new
/// body version and a newly minted fixture; the published fixture stays readable.
constexpr std::string_view frozenCurrentPaneRecordName = "6b76710f4379439cbb9afaaef26fa3a59a1701d8";
/// The v0.3.2 record retained independently of the current encoder fixture.
/// Its provenance and minting command are in Tests/Corpus/sync/handover-release.md.
constexpr std::string_view frozenPublishedPaneRecordName = "6b76710f4379439cbb9afaaef26fa3a59a1701d8";

/// The state the frozen record was written from, rebuilt the same way.
struct FrozenPane {
    Ubiquitty::Terminal terminal;
    HandoverInputWriter writer;
    std::unique_ptr<PaneSource> source;
    SessionFacts session;

    FrozenPane()
    {
        Ubiquitty::resize(terminal, 24, 80);
        // The record was minted with the CRT saver set; the mode starts reset.
        terminal.modes.shouldSaveCRT = true;
        for (int line = 0; line < 40; ++line)
            UbiquittyTest::feed(terminal, "frozen line " + std::to_string(line) + "\r\n");
        UbiquittyTest::feed(terminal, "\033[1;31mred\033[0m \033]8;;https://example.test/frozen\033\\link\033]8;;\033\\ e\xcc\x81 \xe4\xb8\xad\r\n$ ");
        source = std::make_unique<PaneSource>(terminal, ID128 { 17, 18 }, InputPolicy { 100, 50, maxPendingInputBytes });
        source->setInputWriter(writer);
        const ID128 attachment { 31, 32 };
        source->geometry().noteViewport(attachment, GeometryStanding::Driver, { 24, 80 }, 50);
        REQUIRE(source->queueInput({ 21, 22 }, 1, RawInput { { 'l', 's' } }, 100, attachment) == EnqueueOutcome::Queued);
        source->noteReadWouldBlock(200);
        PaneFacts facts;
        facts.programTitle = "frozen pane";
        facts.rows = terminal.rows;
        facts.columns = terminal.columns;
        source->setFacts(facts);
        source->publish(FrameClass::Settled, 300);
        session.sessionID = 17;
        session.createdUnix = 50;
        session.describedStructuralGeneration = source->structuralGeneration();
    }

    [[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encode() const
    {
        const auto unwritten = Ubiquitty::PAL::OutputQueue::Unwritten::copyOf({}, {});
        if (!unwritten)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        return encodePaneHandover(*source, session, *unwritten, paneLimits);
    }
};

std::filesystem::path frozenPaneRecordPath(std::string_view name = frozenCurrentPaneRecordName)
{
#ifdef UBQ_HANDOVER_FIXTURE_DIR
    return std::filesystem::path(UBQ_HANDOVER_FIXTURE_DIR) / std::string(name);
#else
    (void)name;
    return {};
#endif
}

std::optional<std::vector<std::uint8_t>> readFrozenPaneRecord(std::string_view name = frozenCurrentPaneRecordName)
{
    std::ifstream file(frozenPaneRecordPath(name), std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST(theFrozenCurrentPaneRecordRestores)
{
    const auto frozen = readFrozenPaneRecord();
    REQUIRE_MSG(frozen.has_value(), frozenPaneRecordPath().c_str());
    auto restored = decodePaneHandover(*frozen, paneLimits);
    REQUIRE(restored);
    CHECK((restored->source->paneID() == ID128 { 17, 18 }));
    CHECK(restored->source->facts().programTitle == "frozen pane");
    CHECK_EQ(restored->source->inputLedger().lastWritten({ 21, 22 }).value_or(0), 1u);
    CHECK_EQ(restored->session.sessionID, 17u);
    CHECK(!restored->session.firstInputPrincipal);
    CHECK(!restored->session.hasMultipleInputPrincipals);
    FrozenPane original;
    CHECK(UbiquittyTest::firstDisagreement(original.terminal, *restored->terminal).empty());
    CHECK_EQ(restored->terminal->scrollbackCount, original.terminal.scrollbackCount);
}

TEST(theLastPublishedHandoverFormatRemainsReadable)
{
    CHECK(oldestHandoverBodyVersion <= lastPublishedHandoverBodyVersion);
    CHECK(lastPublishedHandoverBodyVersion <= handoverBodyVersion);
    const auto frozen = readFrozenPaneRecord(frozenPublishedPaneRecordName);
    REQUIRE(frozen);
    WireReader reader(*frozen);
    CHECK_EQ(reader.u16(), lastPublishedHandoverBodyVersion);
    auto restored = decodePaneHandover(*frozen, paneLimits);
    REQUIRE(restored);
    CHECK((restored->source->paneID() == ID128 { 17, 18 }));
    CHECK_EQ(restored->session.sessionID, 17u);
    CHECK(restored->source->facts().programTitle == "frozen pane");
    CHECK_EQ(restored->source->inputLedger().lastWritten({ 21, 22 }).value_or(0), 1u);
    FrozenPane original;
    CHECK(UbiquittyTest::firstDisagreement(original.terminal, *restored->terminal).empty());
    CHECK_EQ(restored->terminal->scrollbackCount, original.terminal.scrollbackCount);
}

TEST(aPaneCannotMixSupportedHandoverGrammars)
{
    const auto original = readFrozenPaneRecord(frozenPublishedPaneRecordName);
    REQUIRE(original);
    for (unsigned index = 0; index < 5; ++index) {
        auto changed = *original;
        WireReader reader(changed);
        const auto version = reader.u16();
        (void)reader.take(16);
        for (unsigned preceding = 0; preceding < index; ++preceding) {
            const auto size = reader.u32();
            (void)reader.take(size);
        }
        const auto size = reader.u32();
        const auto section = reader.take(size);
        REQUIRE(reader.ok() && size >= 18);
        const auto offset = static_cast<std::size_t>(section.data() - changed.data());
        const auto other = version == handoverBodyVersion ? handoverBodyVersion + 1 : handoverBodyVersion;
        writeLittle(changed, offset, other, 2);
        const auto digest = digestOf(std::span(changed).subspan(offset, size - 16));
        writeLittle(changed, offset + size - 16, digest.low, 8);
        writeLittle(changed, offset + size - 8, digest.high, 8);
        refreshImageDigest(changed);
        const auto result = decodePaneHandover(changed, paneLimits);
        REQUIRE(!result);
        CHECK(result.error() == HandoverBodyError::Malformed);
    }
}

// A record in a format newer than this host writes is refused before any of
// it is read, so the host that sent it keeps its sessions.
TEST(aPaneRecordInANewerFormatIsRefusedUnread)
{
    auto record = readFrozenPaneRecord();
    REQUIRE(record.has_value());
    (*record)[0] = static_cast<std::uint8_t>(handoverBodyVersion + 1);
    (*record)[1] = 0;
    const auto refused = decodePaneHandover(*record, paneLimits);
    REQUIRE(!refused);
    CHECK(refused.error() == HandoverBodyError::NewerVersion);
}

// A pane record below the supported release floor is refused whole, so
// its holder keeps the sessions.
TEST(aPaneRecordBelowThePublishedReleaseFloorIsRefused)
{
    auto record = readFrozenPaneRecord();
    REQUIRE(record.has_value());
    for (std::uint16_t version = 1; version < oldestHandoverBodyVersion; ++version) {
        writeLittle(*record, 0, version, 2);
        refreshImageDigest(*record);
        const auto refused = decodePaneHandover(*record, paneLimits);
        REQUIRE(!refused);
        CHECK(refused.error() == HandoverBodyError::Malformed);
    }
}

TEST(graphicsRemainWritableWhenCancellationStorageIsRefused)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 4);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        UbiquittyTest::feed(terminal, "ABCDE");
    }
    CHECK(!terminal.graphicCancellation);
    UbiquittyTest::feed(terminal, "\033T");
    CHECK_EQ(terminal.grid->logicalCellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->codePoint, 'A');
    CHECK_EQ(terminal.grid->logicalCellAt(Ubiquitty::Row(1), Ubiquitty::Column(0))->codePoint, 'E');
    CHECK_EQ(terminal.screens.active->cursorX, 1);
    CHECK_EQ(terminal.screens.active->cursorY, 1);
}

TEST(aRefusedCancellationCaptureCannotPartlyUndoAnInsert)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 4);
    UbiquittyTest::feed(terminal, "ABCD\r\033[4h");
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(4 * sizeof(Ubiquitty::Cell));
        UbiquittyTest::feed(terminal, "X");
    }
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > before);
    UbiquittyTest::feed(terminal, "\033T");
    const std::string_view expected = "XABC";
    for (int column = 0; column < 4; ++column)
        CHECK_EQ(terminal.grid->logicalCellAt(Ubiquitty::Row(0), Ubiquitty::Column(column))->codePoint, static_cast<std::uint32_t>(expected[column]));
    CHECK_EQ(terminal.screens.active->cursorX, 1);
}

TEST(cancellationHandoverRefusesEachBufferWithoutChangingTheDestination)
{
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 2, 4);
    UbiquittyTest::feed(source, "A");
    REQUIRE(source.graphicCancellation);
    const auto encoded = encodeHandoverHostState(source, 65536);
    REQUIRE(encoded);
    const auto& capture = *source.graphicCancellation;
    const std::array sizes { capture.cells.size() * sizeof(Ubiquitty::Cell),
        capture.metadata.size() * sizeof(Ubiquitty::RowMeta),
        capture.operations.size() * sizeof(Ubiquitty::Terminal::GraphicCancellation::Operation) };
    for (const auto size : sizes) {
        Ubiquitty::Terminal destination;
        Ubiquitty::resize(destination, 2, 4);
        UbiquittyTest::feed(destination, "keep");
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(size);
        const auto write = encodeHandoverHostState(source, 65536);
        REQUIRE(!write);
        CHECK(write.error() == HandoverBodyError::AllocationRefused);
        const auto read = restoreHandoverHostState(encoded->view(), destination, 65536);
        REQUIRE(!read);
        CHECK(read.error() == HandoverBodyError::AllocationRefused);
        CHECK_EQ(destination.grid->logicalCellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->codePoint, 'k');
        CHECK(destination.graphicCancellation && destination.graphicCancellation->isValid);
    }
}

TEST(printerControllerStateSurvivesHandoverBetweenCompleteSequences)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?102l\033[5i");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->printMode == 5);
    CHECK(!restored->terminal->shouldIgnorePrinterNUL);
    UbiquittyTest::feed(pane.terminal, "discarded\033[4iZ");
    UbiquittyTest::feed(*restored->terminal, "discarded\033[4iZ");
    CHECK(UbiquittyTest::firstDisagreement(pane.terminal, *restored->terminal).empty());
}

TEST(partialPrinterControllerSequencesBlockHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[5i\033[");
    const auto blocked = pane.encode();
    REQUIRE(!blocked);
    CHECK(blocked.error() == HandoverBodyError::NotAtGround);
    UbiquittyTest::feed(pane.terminal, "4i");
    CHECK(pane.encode().has_value());
}

TEST(independentStatusNationalReplacementModeSurvivesHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?36h\033[2$~\033[1$}\033[?36l\033(A#");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->isStatusNationalReplacementEnabled);
    CHECK(!restored->terminal->modes.nrcMode);
    UbiquittyTest::feed(pane.terminal, "#\033[0$}#");
    UbiquittyTest::feed(*restored->terminal, "#\033[0$}#");
    CHECK(UbiquittyTest::firstDisagreement(pane.terminal, *restored->terminal).empty());
}

TEST(rightToLeftCopyModeSurvivesHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?96h");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->shouldCopyRightToLeft);
    CHECK(UbiquittyTest::firstDisagreement(pane.terminal, *restored->terminal).empty());
}

TEST(rightToLeftWritingSurvivesHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?34h\rAB");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->shouldWriteRightToLeft);
    UbiquittyTest::feed(pane.terminal, "CD");
    UbiquittyTest::feed(*restored->terminal, "CD");
    CHECK(UbiquittyTest::firstDisagreement(pane.terminal, *restored->terminal).empty());
}

TEST(cancelCharacterSurvivesHandover)
{
    for (const std::string_view text : { "\033[1;1HABCD\rX", "\033[1;1HABCD\033[1;2H\033[4hX", "\033[999;999HABC" }) {
        FrozenPane pane;
        UbiquittyTest::feed(pane.terminal, text);
        pane.source->noteReadWouldBlock(400);
        pane.source->publish(FrameClass::Settled, 500);
        const auto encoded = pane.encode();
        REQUIRE(encoded);
        auto restored = decodePaneHandover(encoded->view(), paneLimits);
        REQUIRE(restored);
        CHECK(restored->terminal->graphicCancellation && restored->terminal->graphicCancellation->isValid);
        CHECK(!restored->terminal->graphicCancellation->isCapturing);
        UbiquittyTest::feed(pane.terminal, "\033T");
        UbiquittyTest::feed(*restored->terminal, "\033T");
        CHECK(UbiquittyTest::firstDisagreement(pane.terminal, *restored->terminal).empty());
    }
}

TEST(theLineLimitTheDirectionAndTheStringStartSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[1^\033[60 V\033[1;5f");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->terminal->lineLimit, 60);
    CHECK(restored->terminal->implicitMovementDirection == Ubiquitty::VT::ImplicitMovementDirection::AgainstProgression);
    CHECK_EQ(restored->terminal->stringStartColumn, 4);
    UbiquittyTest::feed(*restored->terminal, "\r");
    CHECK_EQ(restored->terminal->screens.active->cursorX, 59);
}

TEST(theApplicationIdentifierAndTheCursorTextColorSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033]21;cursor_text=#112233\033\\\033]30001\033\\");
    UbiquittyTest::feed(pane.terminal, "\033]176;org.example.mail\033\\\033]21;cursor_text=#445566\033\\");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->hasCursorTextColor);
    CHECK_EQ(restored->terminal->cursorTextRed, 0x4444);
    CHECK_EQ(restored->terminal->cursorTextGreen, 0x5555);
    CHECK_EQ(restored->terminal->cursorTextBlue, 0x6666);
    // The saved palette carries its own.
    REQUIRE(restored->terminal->savedPalettes);
    const auto& saved = (*restored->terminal->savedPalettes)[0];
    CHECK(saved.hasCursorTextColor);
    CHECK(saved.cursorTextColor == (Ubiquitty::PaletteRGB { 0x1111, 0x2222, 0x3333 }));
    UbiquittyTest::feed(*restored->terminal, "\033]176;?\033\\");
    std::array<char, 64> reply {};
    const int length = Ubiquitty::drainOutput(restored->terminal.get(), reply);
    CHECK(std::string_view(reply.data(), static_cast<std::size_t>(length > 0 ? length : 0)) == "\033]176;org.example.mail\033\\");
}

TEST(anApplicationIdentifierOfTheFullCapacitySurvivesHandover)
{
    // The ID is held out of line, and the body still carries it as a buffer of
    // its capacity and a byte count: every one of the 255 bytes comes back.
    FrozenPane pane;
    std::string identifier;
    for (std::size_t index = 0; index < Ubiquitty::Terminal::applicationIdentifierCapacity; ++index)
        identifier += static_cast<char>('a' + index % 26);
    UbiquittyTest::feed(pane.terminal, "\033]176;" + identifier + "\033\\");
    REQUIRE(pane.terminal.applicationIdentifier.view() == identifier);
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->applicationIdentifier.view() == identifier);
}

// VT520 DECAC's normal text colours stand in for the default colours, so a
// successor still recolours them on a later assignment and keeps a colour SGR
// selected explicitly at the assigned index.
TEST(theDECACAssignedDefaultMarksSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[H\033[2J\033[1;1;2,|\033[31mR\033[0mD");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    const Ubiquitty::Cell* explicitRed = UbiquittyTest::cell(*restored->terminal, 0, 0);
    const Ubiquitty::Cell* assigned = UbiquittyTest::cell(*restored->terminal, 0, 1);
    REQUIRE(explicitRed);
    REQUIRE(assigned);
    CHECK(!explicitRed->pen.isForegroundAssignedDefault);
    CHECK(assigned->pen.isForegroundAssignedDefault);
    CHECK(assigned->pen.isBackgroundAssignedDefault);
    UbiquittyTest::feed(*restored->terminal, "\033[1;5;6,|");
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 0)->pen.foregroundIndex, 1);
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 1)->pen.foregroundIndex, 5);
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 1)->pen.backgroundIndex, 6);
}

TEST(theEncoderWritesTheFrozenCurrentPaneRecord)
{
    FrozenPane pane;
    // Diagnostic bytes are fixed independently of the parser's batch sizes.
    constexpr std::string_view trace = "1mrd\033[0m \033]8\033\\lk\033]8\033\\ e\xcc \xe4\xb8\xad\r\n$ ";
    static_assert(trace.size() == 32);
    std::ranges::copy(trace, pane.terminal.integration.traceBytes.begin());
    pane.terminal.integration.traceLength = trace.size();
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    if (const char* mint = std::getenv("UBQ_MINT_HANDOVER_FIXTURE"); mint && *mint) {
        // Writing a fixture is never a passing run.
        std::ofstream(mint, std::ios::binary).write(reinterpret_cast<const char*>(encoded->data.get()), static_cast<std::streamsize>(encoded->size));
        CHECK_MSG(false, "minted a pane record; commit it under its content hash, never over an existing one");
        return;
    }
    const auto frozen = readFrozenPaneRecord(frozenCurrentPaneRecordName);
    REQUIRE_MSG(frozen.has_value(), frozenPaneRecordPath(frozenCurrentPaneRecordName).c_str());
    CHECK_MSG(std::ranges::equal(encoded->view(), *frozen),
        "the pane record's grammar changed: raise handoverBodyVersion and mint the fixture for the new version in place of the frozen one");
}

TEST(paneHandoverKeepsSemanticRoles)
{
    using Ubiquitty::SemanticRole;
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\r\033]133;A\007$ \033]133;B\007ls");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto record = pane.encode();
    REQUIRE(record);
    auto restored = decodePaneHandover(record->view(), paneLimits);
    REQUIRE(restored);
    CHECK(screenCheckpoint(*restored->terminal) == screenCheckpoint(pane.terminal));
    CHECK(restored->terminal->screens.active->pen.semanticRole == SemanticRole::Input);
    UbiquittyTest::feed(pane.terminal, "\033[0mX\033]133;C\007Y");
    UbiquittyTest::feed(*restored->terminal, "\033[0mX\033]133;C\007Y");
    CHECK(screenCheckpoint(*restored->terminal) == screenCheckpoint(pane.terminal));
}

TEST(paneHandoverKeepsIndependentCellProtection)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[H\033[1\"qD\033[0\"q\033VG\033[1\"qB\0337");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto record = pane.encode();
    REQUIRE(record);
    auto restored = decodePaneHandover(record->view(), paneLimits);
    REQUIRE(restored);
    CHECK(screenCheckpoint(*restored->terminal) == screenCheckpoint(pane.terminal));
    UbiquittyTest::feed(*restored->terminal, "\033W\033[0\"q\0338C\033[?2K");
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 0)->codePoint, uint32_t('D'));
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 1)->codePoint, 0u);
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 2)->codePoint, uint32_t('B'));
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 3)->codePoint, uint32_t('C'));
    UbiquittyTest::feed(*restored->terminal, "\033[2K");
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 2)->codePoint, uint32_t('B'));
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 3)->codePoint, uint32_t('C'));
}

TEST(paneHandoverKeepsChecksumSelectionAndSavedPalettes)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[1#y\033]4;1;#123456\007\033[#P\033]4;1;#abcdef\007");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto record = pane.encode();
    REQUIRE(record);
    auto restored = decodePaneHandover(record->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->checksumExtensions == pane.terminal.checksumExtensions);
    REQUIRE(restored->terminal->savedPalettes);
    CHECK(restored->terminal->savedPaletteCount == 1);
    UbiquittyTest::feed(pane.terminal, "\033[#Q");
    UbiquittyTest::feed(*restored->terminal, "\033[#Q");
    CHECK(screenCheckpoint(pane.terminal) == screenCheckpoint(*restored->terminal));
}

TEST(paneHandoverKeepsKeyboardSelection)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?1050h\033[?1035l\033P+p6c696e7578\033\\\033[>6;4m\033[>6;1f");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto record = pane.encode();
    REQUIRE(record);
    auto restored = decodePaneHandover(record->view(), paneLimits);
    REQUIRE(restored);
    CHECK(!restored->terminal->modes.shouldHonorNumLock);
    CHECK(restored->terminal->modes.keyboardType == Ubiquitty::Keys::KeyboardType::Termcap);
    CHECK(restored->terminal->modes.termcapProfile == Ubiquitty::Keys::TermcapProfile::Linux);
    CHECK(restored->terminal->xtermKeyModifiers[6] == 4);
    CHECK(restored->terminal->xtermKeyFormats[6] == 1);
    CHECK(screenCheckpoint(pane.terminal) == screenCheckpoint(*restored->terminal));
}

UBIQUITTY_TEST_MAIN()

TEST(lineHomeSurvivesHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[7 U");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->terminal->lineHome, 6);
    UbiquittyTest::feed(*restored->terminal, "\rX");
    CHECK_EQ(restored->terminal->screens.active->cursorX, 7);
}

TEST(progressPropertiesSurviveHandoverBeforeTheirValueIsSet)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033]666;vte.progress.hint=4\033\\");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->terminal->termpropProgressHint, 4);
    CHECK_EQ(restored->terminal->termpropProgressValue, -1);
    UbiquittyTest::feed(*restored->terminal, "\033]666;vte.progress.value=70\033\\");
    CHECK_EQ(restored->terminal->progressState, 4);
    CHECK_EQ(restored->terminal->progressValue, 70);
}

TEST(readlineMouseModesSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?2001;2002;2003h");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->modes.shouldMoveReadlinePointOnClick);
    CHECK(restored->terminal->modes.shouldMoveReadlinePointBeforePaste);
    CHECK(restored->terminal->modes.shouldDeleteReadlineSelection);
}

TEST(alignedTabStopsSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[3g\033[9 a\033[17;46 c");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    REQUIRE(restored->terminal->tabAlignments.size() == Ubiquitty::maxColumns);
    CHECK(restored->terminal->tabAlignments[8] == Ubiquitty::CharacterTabAlignment::LeadingEdge);
    CHECK(restored->terminal->tabAlignments[16] == Ubiquitty::CharacterTabAlignment::TargetCharacter);
    CHECK_EQ(restored->terminal->tabAlignmentTargets[16], uint32_t('.'));
    UbiquittyTest::feed(*restored->terminal, "\033[H\tABC\t12.34\r");
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 6)->codePoint, uint32_t('A'));
    CHECK_EQ(UbiquittyTest::cell(*restored->terminal, 0, 16)->codePoint, uint32_t('.'));
}

TEST(screenPointerStacksAndLineSemanticsSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033]22;>pointer,text\007\033]133;I\007\033[?47h\033]22;>wait,n-resize\007");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->mousePointerShapeStack == pane.terminal.mousePointerShapeStack);
    CHECK(restored->terminal->alternateMousePointerShapeStack == pane.terminal.alternateMousePointerShapeStack);
    CHECK_EQ(restored->terminal->mousePointerShapeStackLength, 2);
    CHECK_EQ(restored->terminal->alternateMousePointerShapeStackLength, 2);
    CHECK(restored->terminal->shouldEndSemanticRoleAtLineBreak);
    UbiquittyTest::feed(*restored->terminal, "\033[?47lI\r\nO");
    CHECK(restored->terminal->screens.active->pen.semanticRole == Ubiquitty::SemanticRole::Output);
    CHECK(!restored->terminal->shouldEndSemanticRoleAtLineBreak);
}

TEST(nestedShellCommandsContinueAfterHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033]133;N;aid=shell\a$\033]133;C\a\r\n");
    pane.terminal.hasCommandStartTick = true;
    pane.terminal.commandStartTick = 123;
    UbiquittyTest::feed(pane.terminal, "\033]133;N;aid=repl\a>\033]133;C\a\r\n");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK_EQ(restored->terminal->shellCommandCount, 2);
    CHECK_EQ((*restored->terminal->shellCommands)[0].startTick, 123u);
    UbiquittyTest::feed(*restored->terminal, "\033]133;N;aid=shell\a");
    CHECK_EQ(restored->terminal->commandCompletedGeneration, 2u);
    CHECK_EQ(restored->terminal->shellCommandCount, 1);
    CHECK_EQ(restored->terminal->pendingPromptRow, Ubiquitty::Screen::scrollbackCount(restored->terminal.get()) + restored->terminal->screens.active->cursorY);
}

TEST(hostStateHandoverRejectsInvalidShellCommandFrames)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "\033]133;N;aid=shell\a");
    source.shellCommandCount = Ubiquitty::Terminal::shellCommandCapacity + 1;
    CHECK(!encodeHandoverHostState(source, hostStateLimit));
    source.shellCommandCount = 1;
    (*source.shellCommands)[0].applicationIDLength = 257;
    CHECK(!encodeHandoverHostState(source, hostStateLimit));
    (*source.shellCommands)[0].applicationIDLength = 1;
    (*source.shellCommands)[0].applicationID[0] = '\033';
    CHECK(!encodeHandoverHostState(source, hostStateLimit));
}

TEST(shellCommandAllocationRefusalDoesNotChangeTheCurrentPrompt)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "X");
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(std::array<Ubiquitty::Terminal::ShellCommand, Ubiquitty::Terminal::shellCommandCapacity>));
        UbiquittyTest::feed(source, "\033]133;N;aid=shell\a");
    }
    CHECK_EQ(source.shellCommandCount, 0);
    CHECK_EQ(source.screens.active->cursorX, 1);
    CHECK_EQ(source.screens.active->cursorY, 0);
    UbiquittyTest::feed(source, "\033]133;N;aid=shell\a");
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal successor;
    successor.answerback = "destination";
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(std::array<Ubiquitty::Terminal::ShellCommand, Ubiquitty::Terminal::shellCommandCapacity>));
        CHECK(restoreHandoverHostState(bytes->view(), successor, hostStateLimit).error() == HandoverBodyError::AllocationRefused);
    }
    CHECK_EQ(successor.shellCommandCount, 0);
    CHECK(!successor.shellCommands);
    CHECK(successor.answerback == "destination");
}

TEST(readlineEraseFactsSurviveHandover)
{
    for (const std::optional<std::uint8_t> erase : { std::optional<std::uint8_t> { 8 }, std::optional<std::uint8_t> {} }) {
        FrozenPane pane;
        PaneEchoFacts facts;
        facts.eraseCharacter = erase;
        pane.session.publishedEchoFacts = facts;
        pane.source->noteEchoFacts(facts);
        pane.source->noteReadWouldBlock(400);
        pane.source->publish(FrameClass::Settled, 500);
        const auto encoded = pane.encode();
        REQUIRE(encoded);
        auto restored = decodePaneHandover(encoded->view(), paneLimits);
        REQUIRE(restored);
        REQUIRE(restored->session.publishedEchoFacts);
        CHECK(restored->session.publishedEchoFacts->eraseCharacter == erase);
        CHECK(restored->source->echoContextFor({}).eraseCharacter == erase);
    }
}

TEST(pasteAndCRTStateSurviveHandover)
{
    FrozenPane pane;
    pane.terminal.modes.shouldReportPasteEvents = true;
    pane.terminal.screenBlanked = true;
    pane.terminal.isCRTBlanked = true;
    pane.terminal.hasDisplayActivityTick = true;
    pane.terminal.displayActivityTick = 123;
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->modes.shouldReportPasteEvents);
    CHECK(restored->terminal->screenBlanked);
    CHECK(restored->terminal->isCRTBlanked);
    CHECK(restored->terminal->hasDisplayActivityTick);
    CHECK(restored->terminal->displayActivityTick == 123);
}

TEST(answerbackConcealmentSurvivesHandover)
{
    FrozenPane pane;
    Ubiquitty::setAnswerbackString(pane.terminal, "answerback");
    UbiquittyTest::feed(pane.terminal, "\033[?101h");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->isAnswerbackConcealed);
    CHECK(restored->terminal->answerback == "answerback");
    CHECK(Ubiquitty::answerbackForDisplay(*restored->terminal) == "concealed");
}

// DECATC's table is what DECRQSS reports, so a successor host keeps it, as it
// keeps the cursor coupling modes a program set.
TEST(alternateTextColorsSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[1;15;4,}\033[15;9;1,}\033[?60h\033[?64l");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(Ubiquitty::alternateTextColorTable(*restored->terminal)[1] == (Ubiquitty::ColorAssignment { 15, 4 }));
    CHECK(Ubiquitty::alternateTextColorTable(*restored->terminal)[15] == (Ubiquitty::ColorAssignment { 9, 1 }));
    CHECK(!Ubiquitty::isColorAssigned(Ubiquitty::alternateTextColorTable(*restored->terminal)[0]));
    CHECK(restored->terminal->modes.isHorizontalCursorCoupled);
    CHECK(!restored->terminal->modes.isVerticalCursorUncoupled);
    CHECK(restored->terminal->modes.isPageCursorUncoupled);
}

// DECAC's assignments are what later SGR resets resolve to and what DECRQSS
// reports, so a successor host keeps them.
TEST(colorAssignmentsSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[1;1;2,|\033[2;3;4,|");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->normalTextColors == (Ubiquitty::ColorAssignment { 1, 2 }));
    CHECK(restored->terminal->windowFrameColors == (Ubiquitty::ColorAssignment { 3, 4 }));
}

TEST(keyPositionModeAndModifierActionsSurviveHandover)
{
    FrozenPane pane;
    UbiquittyTest::feed(pane.terminal, "\033[?81h\033[1;2;6;3+r");
    pane.source->noteReadWouldBlock(400);
    pane.source->publish(FrameClass::Settled, 500);
    const auto encoded = pane.encode();
    REQUIRE(encoded);
    auto restored = decodePaneHandover(encoded->view(), paneLimits);
    REQUIRE(restored);
    CHECK(restored->terminal->modes.shouldReportKeyPositions);
    CHECK(restored->terminal->keyPositionReportedModifiers == 0x11);
    CHECK(restored->terminal->keyPositionDisabledModifiers == 0x20);
}
