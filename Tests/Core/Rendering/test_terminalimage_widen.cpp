// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_terminalimage_widen.cpp — pins the texture-upload-cache helpers shared by
// the Metal, Vulkan (VulkanImageTextures), and Software image
// paths: Image::widenToRGBA8 (RGB24 → straight RGBA8, alpha 255) and the
// imageID-cache staleness primitives (imageContentStamp /
// imageCacheStampMatches).
//
// All three backends call the one widen loop, so a defect in it is a defect in
// every backend and no backend-local test can catch it — these checks are the
// only thing reading that copy. Header-only (inline helpers in
// Image/TerminalImage.h, pure over plain byte buffers), so it links standalone — the
// GPU upload itself can't run headless and is not exercised here.

#include "Assertions.h"

#include "Core/Image/TerminalImage.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static void testWidenRGB24SetsAlpha255()
{
    // 2x2 RGB. Distinct per-pixel colors so a row/stride bug shows up.
    const unsigned char rgb[2 * 2 * 3] = {
        10, 20, 30, /* (0,0) */ 40, 50, 60, // (1,0)
        70, 80, 90, /* (0,1) */ 100, 110, 120 // (1,1)
    };
    std::vector<unsigned char> outputBuffer(2 * 2 * 4, 0xAB);
    Ubiquitty::Image::widenToRGBA8(rgb, 2, 2, 24, outputBuffer.data());
    const unsigned char want[2 * 2 * 4] = { 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255 };
    for (int i = 0; i < 2 * 2 * 4; ++i)
        CHECK_MSG(outputBuffer[i] == want[i], "rgb24 widen byte mismatch");
}

static void testWidenRgba32PassesAlphaThrough()
{
    const unsigned char rgba[1 * 2 * 4] = {
        1, 2, 3, 4, /* (0,0) */ 5, 6, 7, 8 // (0,1)
    };
    std::vector<unsigned char> outputBuffer(1 * 2 * 4, 0);
    Ubiquitty::Image::widenToRGBA8(rgba, 1, 2, 32, outputBuffer.data());
    for (int i = 0; i < 1 * 2 * 4; ++i)
        CHECK_MSG(outputBuffer[i] == rgba[i], "rgba32 widen should be a straight copy");
}

// Deterministic, stride-sensitive fill so any lane shuffle or offset bug in the
// vectorized widen shows as a byte mismatch.
static unsigned char patternByte(size_t index)
{
    return static_cast<unsigned char>((index * 131 + 17) & 0xFF);
}

// The t1 direct-upload optimization replaceRegions format-32 sourcePixels
// straight to the texture; that is only sound if widenToRGBA8 on format 32 is
// byte-identical to the source (the memcpy fast path) across a width>1,
// height>1 buffer.
static void testWidenRgba32DirectUploadByteIdentical()
{
    const int width = 3;
    const int height = 2;
    unsigned char rgba[3 * 2 * 4];
    for (size_t i = 0; i < sizeof(rgba); ++i)
        rgba[i] = patternByte(i);
    std::vector<unsigned char> widened(3 * 2 * 4, 0xCD);
    Ubiquitty::Image::widenToRGBA8(rgba, width, height, 32, widened.data());
    for (size_t i = 0; i < sizeof(rgba); ++i)
        CHECK_MSG(widened[i] == rgba[i], "rgba32 widen must be byte-identical to the source");
}

// Pins the RGB24 pixel loop (auto-vectorized) at a pixel count that exercises
// both a full vector body and a scalar tail under any vector width the
// compiler picks (37x3 = 111 pixels, not a multiple of 8/16/32).
static void testWidenRGB24VectorBodyAndTail()
{
    const int width = 37;
    const int height = 3;
    std::vector<unsigned char> rgb(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < rgb.size(); ++i)
        rgb[i] = patternByte(i);
    std::vector<unsigned char> outputBuffer(static_cast<size_t>(width) * height * 4, 0xEE);
    Ubiquitty::Image::widenToRGBA8(rgb.data(), width, height, 24, outputBuffer.data());
    for (int pixel = 0; pixel < width * height; ++pixel) {
        CHECK_MSG(outputBuffer[pixel * 4 + 0] == rgb[pixel * 3 + 0], "rgb24 vector widen: red channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 1] == rgb[pixel * 3 + 1], "rgb24 vector widen: green channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 2] == rgb[pixel * 3 + 2], "rgb24 vector widen: blue channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 3] == 255, "rgb24 vector widen: alpha 255");
    }
}

// A pixel count below one vector iteration takes the scalar tail exclusively.
static void testWidenRGB24NarrowImageTailOnly()
{
    const int width = 5;
    const int height = 2;
    std::vector<unsigned char> rgb(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < rgb.size(); ++i)
        rgb[i] = patternByte(i + 7);
    std::vector<unsigned char> outputBuffer(static_cast<size_t>(width) * height * 4, 0x55);
    Ubiquitty::Image::widenToRGBA8(rgb.data(), width, height, 24, outputBuffer.data());
    for (int pixel = 0; pixel < width * height; ++pixel) {
        CHECK_MSG(outputBuffer[pixel * 4 + 0] == rgb[pixel * 3 + 0], "rgb24 tail widen: red channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 1] == rgb[pixel * 3 + 1], "rgb24 tail widen: green channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 2] == rgb[pixel * 3 + 2], "rgb24 tail widen: blue channel");
        CHECK_MSG(outputBuffer[pixel * 4 + 3] == 255, "rgb24 tail widen: alpha 255");
    }
}

static void testWidenUnknownFormatTreatedAsRGB24()
{
    const unsigned char rgb[1 * 1 * 3] = { 9, 8, 7 };
    unsigned char outputBuffer[4] = { 0 };
    Ubiquitty::Image::widenToRGBA8(rgb, 1, 1, 0 /* not 32 → 3 bytes/pixel */, outputBuffer);
    CHECK_MSG(outputBuffer[0] == 9 && outputBuffer[1] == 8 && outputBuffer[2] == 7 && outputBuffer[3] == 255, "unknown format should widen as RGB24");
}

// The image-taking overload reads every field the definition keys on.
//
// This is the form the backends call, and the failure it exists to prevent is a
// silent one: a call site that omitted contentGeneration would still compile,
// still cache, and freeze a kitty animation on frame 1 with nothing to see but
// the wrong pixels. So each field is moved on its own and the stamp must move
// with it -- a field the overload forgets to pass shows up here as a stamp that
// did not change.
static void testStampOverloadReadsEveryField()
{
    using Ubiquitty::Image::imageContentStamp;

    Ubiquitty::Image::TerminalImage image {};
    image.width = 64;
    image.height = 48;
    image.currentFrame = 0;
    image.format = 24;
    image.contentGeneration = 1;

    const uint64_t base = imageContentStamp(image);
    CHECK_MSG(base == imageContentStamp(64, 48, 0, 24, 1), "the overload agrees with the definition it forwards to");

    {
        Ubiquitty::Image::TerminalImage moved = image;
        moved.width = 100;
        CHECK_MSG(imageContentStamp(moved) != base, "width reaches the stamp through the overload");
    }
    {
        Ubiquitty::Image::TerminalImage moved = image;
        moved.height = 100;
        CHECK_MSG(imageContentStamp(moved) != base, "height reaches the stamp through the overload");
    }
    {
        // A kitty frame advance: same id, same size, new pixels.
        Ubiquitty::Image::TerminalImage moved = image;
        moved.currentFrame = 1;
        CHECK_MSG(imageContentStamp(moved) != base, "a frame advance reaches the stamp through the overload");
    }
    {
        Ubiquitty::Image::TerminalImage moved = image;
        moved.format = 32;
        CHECK_MSG(imageContentStamp(moved) != base, "format reaches the stamp through the overload");
    }
    {
        // A same-id re-transmit at identical geometry, frame and format. Every
        // other field is unchanged by construction, so this is the one a cache
        // keyed on geometry cannot see at all.
        Ubiquitty::Image::TerminalImage moved = image;
        moved.contentGeneration = 2;
        CHECK_MSG(imageContentStamp(moved) != base, "a same-size re-transmit reaches the stamp through the overload");
    }
}

static void testStampDistinguishesDimensionsFrameFormat()
{
    using Ubiquitty::Image::imageContentStamp;
    // gen == 1 fixed here; geometry/frame/format variation only.
    const uint64_t base = imageContentStamp(64, 48, 0, 24, 1);
    CHECK_MSG(base != imageContentStamp(48, 64, 0, 24, 1), "width/height swap differs");
    CHECK_MSG(base != imageContentStamp(64, 48, 1, 24, 1), "frame advance differs");
    CHECK_MSG(base != imageContentStamp(64, 48, 0, 32, 1), "format change differs");

    // Pairwise distinct across the whole space, not one field at a time. A
    // stamp is a cache key: what breaks it is not a field being ignored — the
    // three checks above catch that — but two fields sharing bits, so a change
    // in one can be cancelled by a change in the other. No assertion that
    // varies a single field can see a cancellation, because it never varies the
    // second one.
    //
    // format 0 is in the set because it is the "unknown, treat as RGB24"
    // value testWidenUnknownFormatTreatedAsRGB24 passes, so it reaches the
    // stamp in the shipping decoder rather than only here.
    {
        struct Inputs {
            int width, height, frame, format;
            uint64_t generation;
        };
        std::vector<Inputs> space;
        for (int width : { 64, 100 })
            for (int height : { 48, 100 })
                for (int frame : { 0, 1, 2, 3 })
                    for (int format : { 0, 8, 24, 32 })
                        for (uint64_t generation : { uint64_t { 1 }, uint64_t { 2 } })
                            space.push_back({ width, height, frame, format, generation });

        std::size_t collisions = 0;
        for (std::size_t i = 0; i < space.size(); ++i) {
            for (std::size_t j = i + 1; j < space.size(); ++j) {
                const uint64_t a = imageContentStamp(space[i].width, space[i].height, space[i].frame, space[i].format, space[i].generation);
                const uint64_t b = imageContentStamp(space[j].width, space[j].height, space[j].frame, space[j].format, space[j].generation);
                if (a != b)
                    continue;
                if (collisions++ < 3)
                    std::printf("  stamp collision: (%d,%d,f%d,fmt%d,gen%llu) == (%d,%d,f%d,fmt%d,gen%llu) -> %llx\n",
                        space[i].width, space[i].height, space[i].frame, space[i].format, static_cast<unsigned long long>(space[i].generation),
                        space[j].width, space[j].height, space[j].frame, space[j].format, static_cast<unsigned long long>(space[j].generation),
                        static_cast<unsigned long long>(a));
            }
        }
        CHECK_MSG(collisions == 0, "every distinct image content gets its own stamp");
        // A sweep that compared nothing would report no collisions too.
        CHECK(space.size() == 2u * 2u * 4u * 4u * 2u);
    }
}

static void testStampDistinguishesContentGeneration()
{
    using Ubiquitty::Image::imageContentStamp;
    // A same-id re-transmit keeps width/height/currentFrame/format identical
    // but replaces the pixel bytes; only contentGeneration moves. The
    // stamp must change or the cached texture (frame 1) is never re-uploaded.
    const uint64_t genA = imageContentStamp(2, 2, 0, 32, 1);
    const uint64_t genB = imageContentStamp(2, 2, 0, 32, 2);
    CHECK_MSG(genA != genB, "same geometry/frame/format, new generation → different stamp");
    // The golden-ratio mix must not let an adjacent generation alias a geometry
    // bit: neither generation should collide with a plain geometry-only stamp.
    CHECK_MSG(genA != imageContentStamp(2, 2, 0, 32, 0), "generation 0 vs 1 differ");
    CHECK_MSG(genB != imageContentStamp(2, 2, 0, 32, 0), "generation 0 vs 2 differ");
}

static void testCacheStampMatches()
{
    using Ubiquitty::Image::imageCacheStampMatches;
    using Ubiquitty::Image::imageContentStamp;
    using Ubiquitty::Image::TextureState;
    const uint64_t stored = imageContentStamp(32, 32, 0, 24, 1);
    // No texture yet → never a match (must upload).
    CHECK_MSG(!imageCacheStampMatches(TextureState::NoTexture, stored, 32, 32, 0, 24, 1), "no texture → upload");
    // Texture + matching stamp → reuse.
    CHECK_MSG(imageCacheStampMatches(TextureState::HasTexture, stored, 32, 32, 0, 24, 1), "match → reuse");
    // Texture but a frame advanced → stale, must re-upload.
    CHECK_MSG(!imageCacheStampMatches(TextureState::HasTexture, stored, 32, 32, 1, 24, 1), "frame advance → re-upload");
    // Texture but a same-id re-decode at a new size → stale.
    CHECK_MSG(!imageCacheStampMatches(TextureState::HasTexture, stored, 64, 32, 0, 24, 1), "resize → re-upload");
    // A same-id re-transmit — identical geometry/frame/format, new
    // generation. A geometry-only stamp reports a match here and freezes on
    // frame 1; the generation-aware stamp must report stale.
    CHECK_MSG(!imageCacheStampMatches(TextureState::HasTexture, stored, 32, 32, 0, 24, 2), "same-id re-transmit (new generation) → re-upload");
}

int main()
{
    testWidenRGB24SetsAlpha255();
    testWidenRgba32PassesAlphaThrough();
    testWidenRgba32DirectUploadByteIdentical();
    testWidenRGB24VectorBodyAndTail();
    testWidenRGB24NarrowImageTailOnly();
    testWidenUnknownFormatTreatedAsRGB24();
    testStampDistinguishesDimensionsFrameFormat();
    testStampOverloadReadsEveryField();
    testStampDistinguishesContentGeneration();
    testCacheStampMatches();

    if (failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    std::printf("test_terminalimage_widen: ok\n");
    return 0;
}
