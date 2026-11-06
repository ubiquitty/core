// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Samples.h"

#include <array>
#include <new>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

constexpr std::array<std::int16_t, 4> sourceFrames { -32768, -1, 0, 32767 };

SampleBytes capture()
{
    auto bytes = SampleBytes::capture(7, sourceFrames, sourceFrames.size() * 2);
    CHECK(bytes.has_value());
    return std::move(*bytes);
}

BulkChunk finalChunk(const SampleBytes& bytes, std::uint32_t transfer = 3)
{
    return { .transfer = transfer, .index = 0, .isLast = true, .bytes = bytes.view(), .sha256 = bytes.sample.sha256 };
}

TEST(sampleCaptureIsCanonicalAndIndependentOfTheBank)
{
    auto values = sourceFrames;
    auto bytes = SampleBytes::capture(7, values, 8);
    CHECK(bytes);
    const std::array<std::uint8_t, 8> expected { 0, 128, 255, 255, 0, 0, 255, 127 };
    CHECK(std::ranges::equal(bytes->view(), expected));
    // Independent SHA256 of the eight canonical little-endian bytes above.
    constexpr std::array<std::uint8_t, 32> digest { 0xa0, 0xb3, 0xd8, 0xf5, 0xdb, 0xcd, 0xc6, 0xda,
        0xc3, 0x37, 0xb3, 0xde, 0x2e, 0x92, 0xfe, 0x62, 0xe1, 0x39, 0x1e, 0x6a, 0x03, 0x1d,
        0x57, 0x7c, 0xa3, 0xf7, 0xb4, 0xb8, 0x04, 0xac, 0x9c, 0x79 };
    CHECK(bytes->sample.sha256 == digest);
    values.fill(8);
    CHECK(std::ranges::equal(bytes->view(), expected));
    CHECK_EQ(bytes->sample.serial, 7u);
    CHECK_EQ(bytes->sample.frames, 4u);
    CHECK(!SampleBytes::capture(7, sourceFrames, 7));
}

TEST(sampleHeadIsExactAndBounded)
{
    const auto bytes = capture();
    const SampleHead head { .slot = 25, .sample = bytes.sample, .transfer = 3 };
    std::vector<std::uint8_t> encoded;
    CHECK(encodeSampleHead(encoded, head));
    CHECK_EQ(encoded.size(), 45u);
    const auto decoded = decodeSampleHead(encoded);
    CHECK(decoded && decoded->slot == 25 && decoded->sample == bytes.sample && decoded->transfer == 3);
    for (std::size_t size = 0; size < encoded.size(); ++size)
        CHECK(!decodeSampleHead(std::span(encoded).first(size)));
    encoded.push_back(0);
    CHECK(!decodeSampleHead(encoded));
    for (const auto slot : { 0, 26, 255 }) {
        encoded.clear();
        CHECK(!encodeSampleHead(encoded, { .slot = static_cast<std::uint8_t>(slot), .sample = bytes.sample, .transfer = 3 }));
        CHECK(encoded.empty());
    }
}

TEST(emptySampleIsAnExplicitClearWithoutAnAllocation)
{
    auto bytes = SampleBytes::capture(0, {}, 0);
    CHECK(bytes && bytes->view().empty() && !bytes->bytes);
    std::vector<std::uint8_t> encoded;
    CHECK(encodeSampleHead(encoded, { .slot = 1, .sample = bytes->sample, .transfer = 0 }));
    CHECK(decodeSampleHead(encoded));
    CHECK(!SampleTransfer::begin({ .slot = 1, .sample = bytes->sample, .transfer = 0 }, 0));
    CHECK(!encodeSampleHead(encoded, { .slot = 1, .sample = bytes->sample, .transfer = 3 }));
    bytes->sample.sha256[0] = 1;
    CHECK(!isValidSampleIdentity(bytes->sample));
}

TEST(sampleFrameLimitIsNotSilentlyTruncated)
{
    std::vector<std::int16_t> values(Ubiquitty::VT::sampleMaxFrames);
    auto bytes = SampleBytes::capture(1, values, values.size() * 2);
    CHECK(bytes && bytes->sample.frames == Ubiquitty::VT::sampleMaxFrames);
    values.push_back(0);
    CHECK(!SampleBytes::capture(1, values, values.size() * 2));
    SampleIdentity oversized { .serial = 1, .frames = Ubiquitty::VT::sampleMaxFrames + 1, .sha256 = {} };
    CHECK(!SampleTransfer::begin({ .slot = 1, .sample = oversized, .transfer = 3 }, values.size() * 2));
}

TEST(sampleTransferPublishesOnlyAfterBothHashesMatch)
{
    const auto bytes = capture();
    auto transfer = SampleTransfer::begin({ .slot = 1, .sample = bytes.sample, .transfer = 3 }, 8);
    CHECK(transfer && transfer->heldBytes() == 8);
    CHECK(!transfer->take());
    CHECK(transfer->accept(finalChunk(bytes)) == SampleChunkResult::Complete);
    auto pcm = transfer->take();
    CHECK(pcm && std::ranges::equal(pcm->view(), sourceFrames));
    CHECK_EQ(transfer->heldBytes(), 0u);
    CHECK(!transfer->take());
}

TEST(sampleChunksMaySplitSignedFrames)
{
    const auto bytes = capture();
    for (std::size_t split = 1; split < bytes.view().size(); ++split) {
        auto transfer = SampleTransfer::begin({ .slot = 1, .sample = bytes.sample, .transfer = 3 }, 8);
        CHECK(transfer);
        CHECK(transfer->accept({ .transfer = 3, .index = 0, .isLast = false, .bytes = bytes.view().first(split) }) == SampleChunkResult::Pending);
        CHECK(!transfer->take());
        CHECK_EQ(transfer->heldBytes(), 8u);
        auto final = finalChunk(bytes);
        final.index = 1;
        final.bytes = bytes.view().subspan(split);
        CHECK(transfer->accept(final) == SampleChunkResult::Complete);
        const auto pcm = transfer->take();
        CHECK(pcm && std::ranges::equal(pcm->view(), sourceFrames));
    }
}

TEST(sampleRefusalReleasesTheWholeOwner)
{
    const auto bytes = capture();
    for (int invalid = 0; invalid < 8; ++invalid) {
        auto head = SampleHead { .slot = 1, .sample = bytes.sample, .transfer = 3 };
        if (invalid == 7)
            head.sample.sha256[0] ^= 1;
        auto transfer = SampleTransfer::begin(head, 8);
        CHECK(transfer);
        auto chunk = finalChunk(bytes);
        switch (invalid) {
        case 0:
            chunk.transfer = 4;
            break;
        case 1:
            chunk.index = 1;
            break;
        case 2:
            chunk.bytes = {};
            break;
        case 3:
            chunk.bytes = chunk.bytes.first(7);
            break;
        case 4:
            chunk.isLast = false;
            break;
        case 5:
            chunk.compression = Compression::Deflate;
            break;
        case 6:
            chunk.sha256[0] ^= 1;
            break;
        case 7:
            break;
        }
        CHECK(transfer->accept(chunk) == SampleChunkResult::Refused);
        CHECK_EQ(transfer->heldBytes(), 0u);
        CHECK(!transfer->take());
    }
    CHECK(!SampleTransfer::begin({ .slot = 1, .sample = bytes.sample, .transfer = 3 }, 7));
}

TEST(sampleAllocationFailureDoesNotPublishAnOwner)
{
    const auto bytes = capture();
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal(8);
    CHECK(!SampleBytes::capture(7, sourceFrames, 8));
    CHECK(!SampleTransfer::begin({ .slot = 1, .sample = bytes.sample, .transfer = 3 }, 8));
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() - before, 2u);
}

} // namespace

UBIQUITTY_TEST_MAIN()
