// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The limits every decoded image crosses on its way into the store.
//
// storeRGBAImage is where sixel and OSC 1337 meet, and neither bounds what it
// hands over: sixel clamps its raster upstream, but the PAL image decoder behind
// OSC 1337 reports whatever dimensions it decoded. The kitty paths apply the
// per-image cap before they reach the store and call enforceBudget after, so the
// cases here are about the two ingresses that did neither.

#include "Harness.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdlib>

namespace {

// The store frees pixels with free(), so a test buffer has to come from malloc.
unsigned char* pixelBuffer(std::size_t bytes)
{
    auto* p = static_cast<unsigned char*>(std::malloc(bytes ? bytes : 1));
    CHECK(p != nullptr);
    return p;
}

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used && t.images.stored[i].pixels)
            ++n;
    return n;
}

} // namespace

TEST(testStoreRejectsAnImageOverTheMaxPerImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // maxDecodedImageBytes is the ceiling the kitty decode path applies before the
    // store; the store now applies it for everyone. One pixel past it is refused,
    // and the caller keeps ownership of the buffer it offered.
    const int overMaxSide = 4097; // 4097*4097*4 == 67.1 MB, past the 64 MiB ceiling
    unsigned char* rejected = pixelBuffer(16);
    // Free only on refusal: a refusal leaves ownership with the caller (both real
    // callers free on false), so freeing unconditionally would double-free the
    // moment this check regresses, and a crash reports nothing.
    const bool didStoreOverMaxImage = Ubiquitty::Image::storeRGBAImage(&t, rejected, overMaxSide, overMaxSide, 1, 1);
    CHECK(!didStoreOverMaxImage);
    CHECK(liveImageCount(t) == 0);
    if (!didStoreOverMaxImage)
        std::free(rejected);

    // Just inside the ceiling is accepted.
    const int underMaxSide = 4000; // 4000*4000*4 == 64.0 MB, inside
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), underMaxSide, underMaxSide, 1, 1));
    CHECK(liveImageCount(t) == 1);
}

TEST(testStoreRejectsDimensionsThatWouldTruncate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // width and height are uint16_t fields. 65536 truncates to 0, which every
    // consumer silently skips — the image would vanish rather than be refused.
    unsigned char* wide = pixelBuffer(16);
    const bool storedWide = Ubiquitty::Image::storeRGBAImage(&t, wide, 65536, 1, 1, 1);
    CHECK(!storedWide);
    CHECK(liveImageCount(t) == 0);
    if (!storedWide)
        std::free(wide);

    unsigned char* tall = pixelBuffer(16);
    const bool storedTall = Ubiquitty::Image::storeRGBAImage(&t, tall, 1, 65536, 1, 1);
    CHECK(!storedTall);
    CHECK(liveImageCount(t) == 0);
    if (!storedTall)
        std::free(tall);

    // The largest dimensions the fields hold are still accepted, so the check
    // rejects only what would truncate. 65535x64 stays inside the byte ceiling.
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 65535, 64, 1, 1));
    CHECK(liveImageCount(t) == 1);
}

TEST(testStoreRejectsAByteCountThatWrapsUint32)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // 40000*40000*4 == 6.4e9, which wraps uint32 to ~2.1e9. Computed in 32 bits
    // the wrapped value would look like a plausible length; in 64 bits it is
    // plainly past the ceiling. Both dimensions fit uint16, so only the byte count
    // rejects this one.
    unsigned char* wrapped = pixelBuffer(16);
    const bool storedWrapped = Ubiquitty::Image::storeRGBAImage(&t, wrapped, 40000, 40000, 1, 1);
    CHECK(!storedWrapped);
    CHECK(liveImageCount(t) == 0);
    if (!storedWrapped)
        std::free(wrapped);
}

TEST(testTheByteBudgetStopsAtSixteenMebipixelsOfRGBA)
{
    // 4096 x 4096 RGBA is exactly maxDecodedImageBytes. A row or a column more is
    // well inside the axis ceiling, so only the byte half refuses it.
    CHECK(!Ubiquitty::Image::isDecodedImageBytesRefused(4096, 4096));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(4096, 4097));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(4097, 4096));
    CHECK(!Ubiquitty::Image::isDecodedImageSizeRefused(4097, 4096));

    // The axis half still applies: a sliver inside the bytes is refused past
    // 16384 on either side, and so is an empty or negative one.
    CHECK(!Ubiquitty::Image::isDecodedImageBytesRefused(16384, 1024));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(16385, 1));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(1, 16385));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(0, 1));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(1, -1));

    // 16384 x 16384, which the axis check alone passes, is a gibibyte of RGBA.
    CHECK(!Ubiquitty::Image::isDecodedImageSizeRefused(16384, 16384));
    CHECK(Ubiquitty::Image::isDecodedImageBytesRefused(16384, 16384));
}

TEST(testTheByteBudgetAgreesWithTheProductOnEitherSideOfIt)
{
    // The division has to refuse exactly what the product exceeds, at either
    // depth, or a caller that compared the product before would change verdict.
    constexpr long long budget = Ubiquitty::Image::maxDecodedImageBytes;
    for (const int bytesPerPixel : { 3, 4 }) {
        for (const long long width : { 1024LL, 2048LL, 4095LL, 4096LL, 4097LL, 4729LL, 5000LL, 8191LL, 16383LL, 16384LL }) {
            const long long tallest = budget / bytesPerPixel / width;
            for (long long height = tallest - 1; height <= tallest + 1; ++height) {
                if (height < 1 || height > Ubiquitty::Image::maxDecodedImageDimension)
                    continue;
                const bool isOverBudget = width * height * bytesPerPixel > budget;
                CHECK_EQ(Ubiquitty::Image::isDecodedImageBytesRefused(width, height, bytesPerPixel), isOverBudget);
            }
        }
    }
}

TEST(testStoreEvictsToStayInsideTheBudget)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // A budget that two images exceed but one does not. Each image below is
    // 1000*1000*4 == 4 MB.
    t.images.kittyImageBytesBudget = 6u * 1000u * 1000u;

    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 1000, 1000, 1, 1));
    CHECK(liveImageCount(t) == 1);

    // Storing the second must evict the first rather than hold both: without the
    // enforceBudget call the store kept every image up to the slot count,
    // whatever the budget said.
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 1000, 1000, 1, 1));
    CHECK(liveImageCount(t) == 1);
    CHECK(Ubiquitty::Image::totalBytes(&t) <= t.images.kittyImageBytesBudget);
}

UBIQUITTY_TEST_MAIN()
