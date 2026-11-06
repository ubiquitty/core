// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_decode_buffer_release — what an image payload's buffers keep afterwards.
//
// The kitty APC frame buffer, the chunked-transfer buffer and the sixel
// accumulator each double toward a ceiling of 16 or 64 MiB as a payload
// arrives. Emptied by length alone, each kept the largest allocation it had
// ever reached for as long as the terminal lived, so one large image held
// megabytes in every pane that had shown one. These cases send payloads past
// the retained bound and read the capacity left once each is dispatched,
// abandoned or refused, and check that an ordinary payload keeps its buffer.

#include "Core/Kitty/Graphics.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "KittyGraphicsFixture.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makeRGBA;

constexpr int mebibyte = 1024 * 1024;

void prepare(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
}

/// The base64 of a 512 by 512 RGBA image: 1 MiB of pixels, 1.33 MiB on the wire.
std::string oneMebibyteOfPixels()
{
    const std::vector<std::uint8_t> pixels = makeRGBA(512, 512);
    return base64(pixels);
}

} // namespace

TEST(testAOneMebibyteAPCReleasesItsFrameBufferOnceDispatched)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::string payload = oneMebibyteOfPixels();
    REQUIRE(payload.size() > static_cast<std::size_t>(mebibyte));

    feed(t, apc("a=t,f=32,s=512,v=512,i=7,q=2", payload));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
    CHECK(t.images.kittyAPC.length == 0);

    // A block whose introducer is not `G` takes the same frame buffer.
    feed(t, "\033_X" + std::string(static_cast<std::size_t>(mebibyte), 'a') + "\033\\");
    CHECK(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
}

TEST(testAnAPCPastItsMaximumReleasesTheBufferItOverflowed)
{
    Ubiquitty::Terminal t;
    prepare(t);
    // Delivered a mebibyte at a time, as a PTY read would, so the buffer grows
    // to its ceiling before the byte that passes it.
    const std::string slice(static_cast<std::size_t>(mebibyte), 'A');
    feed(t, "\033_G");
    for (int read = 0; read <= Ubiquitty::Kitty::maxAPCCapacity / mebibyte; ++read)
        feed(t, slice);
    REQUIRE(t.images.kittyAPCOverflow);
    feed(t, "\033\\");
    CHECK(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
}

TEST(testAnOrdinaryAPCKeepsItsFrameBuffer)
{
    Ubiquitty::Terminal t;
    prepare(t);
    feed(t, apc("a=t,f=32,s=8,v=8,i=3,q=2", base64(makeRGBA(8, 8))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(t.images.kittyAPC.capacity > 0);
    CHECK(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
}

TEST(testAChunkedTransferReleasesItsChunkBufferOnceComplete)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::string payload = oneMebibyteOfPixels();
    constexpr std::size_t block = 4096;
    for (std::size_t offset = 0; offset < payload.size(); offset += block) {
        const bool isLast = offset + block >= payload.size();
        const std::string keys = offset == 0 ? "a=t,f=32,s=512,v=512,i=9,q=2,m=1" : (isLast ? "m=0" : "m=1");
        feed(t, apc(keys, payload.substr(offset, block)));
        if (!isLast)
            REQUIRE(t.images.kittyChunkActive);
    }
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(!t.images.kittyChunkActive);
    CHECK(t.images.kittyChunk.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
    CHECK(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
}

TEST(testAnAbandonedChunkedTransferReleasesItsChunkBuffer)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::string payload = oneMebibyteOfPixels();
    constexpr std::size_t block = 4096;
    for (std::size_t offset = 0; offset + block < payload.size(); offset += block)
        feed(t, apc(offset == 0 ? "a=t,f=32,s=512,v=512,i=11,q=2,m=1" : "m=1", payload.substr(offset, block)));
    REQUIRE(t.images.kittyChunkActive);
    REQUIRE(t.images.kittyChunk.capacity > Ubiquitty::Kitty::maxRetainedAPCCapacity);

    // A command of its own between blocks abandons the open transfer.
    feed(t, "\033_Ga=d,d=a,q=2\033\\");
    CHECK(!t.images.kittyChunkActive);
    CHECK(t.images.kittyChunk.capacity <= Ubiquitty::Kitty::maxRetainedAPCCapacity);
}

TEST(testALargeSixelReleasesItsAccumulatorOnceComplete)
{
    Ubiquitty::Terminal t;
    prepare(t);
    // Seventy bands of a thousand sixels: about 70 KB, past the accumulator's
    // initial allocation.
    std::string sixel = "\033Pq#0;2;100;0;0#0";
    for (int band = 0; band < 70; ++band)
        sixel += std::string(1000, '~') + "-";
    sixel += "\033\\";
    feed(t, sixel);
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(t.images.sixelAccumulatorCapacity <= Ubiquitty::VT::sixelAccumulatorInitialCapacity);
    CHECK(t.images.sixelAccumulatorLength == 0);

    // The next sixel reserves the initial allocation again.
    feed(t, "\033Pq#0;2;0;100;0#0~~~~\033\\");
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(t.images.sixelAccumulatorCapacity == Ubiquitty::VT::sixelAccumulatorInitialCapacity);
}

UBIQUITTY_TEST_MAIN()
