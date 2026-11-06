// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The image protocols, and the pixel state they leave behind.
//
// The kitty graphics protocol in full — transmission, placement, chunking,
// compression, deletion and the id space that ties them together — plus sixel,
// iTerm2's inline images, and the Tek 4014 vector mode.
//
// One file because they compete for the same resources: an image store with a
// pixel budget, a placement list anchored to cells that scroll, and an eviction
// policy that has to choose between them. A test that covers one protocol in
// isolation cannot see the case where two of them disagree about what a cell
// holds.

#include "Harness.h"
#include "VT100Fixture.h"
#include <span>

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Image/TerminalImage.h"
#include "Core/VT/Parser.h"

#include <string>
#include <vector>

/// Whether findKittyPlacement also has to match the placement ID.
enum class PlacementIDMatch {
    Ignore,
    Require,
};

/// Returns the first placement with this image ID, or nullptr when none matches.
///
/// With `PlacementIDMatch::Require` the placement ID must match too, which is
/// how the geometry tests distinguish several placements of one image.
static const Ubiquitty::Image::TerminalImagePlacement* findKittyPlacement(Ubiquitty::Terminal& t, uint32_t imageID, PlacementIDMatch idMatch, uint32_t placementID)
{
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImagePlacementCount; i++) {
        const Ubiquitty::Image::TerminalImagePlacement* p = Ubiquitty::Image::placement(&t, i);
        if (!p || p->imageID != imageID)
            continue;
        if (idMatch == PlacementIDMatch::Require && p->placementID != placementID)
            continue;
        return p;
    }
    return nullptr;
}

static void installFakeFileSystem(Ubiquitty::Terminal& t, const char* path, const unsigned char* bytes, int length)
{
    fakeFileSystemPath = path;
    fakeFileSystemBytes = bytes;
    fakeFileSystemLength = length;
    fakeFileSystemUnlinkCount = 0;
    t.fileReadClient = &fakeFileReadClient;
}

/// A denying policy gates the t=f read (FileReadAccess, no image installed); an
/// allowing policy lets it through. An unset policy answers PAL's default, which
/// allows the default Local origin (covered above).
/// ProgramKeys: a denying policy blocks DECUDK key reprogramming (the
/// input-injection defense — a remapped key would emit attacker bytes when
/// pressed); an allowing policy lets the program through.
/// a=q (query support) must ATTEMPT the load — honoring f= / t= / o= + payload — and
/// reply OK or the SPECIFIC error, without storing the image or unlinking a t=t temp
/// file (kitty-aq-query-does-not-validate-load). Short-circuiting to an
/// id-existence check validates nothing, and a fresh-id probe always ENOENTs.
/// t=t (temp file). Same shape as t=f but expects the hook's
/// shouldUnlink flag to fire — the PAL is responsible for removing the
/// file once the data is in hand.
/// t=s (shared memory). The parser does not map shared memory, so it
/// must respond with a kitty-protocol error and not paint.
// o=z compression — same 2×1 RGBA payload as the format_24_vs_32 test
// above, but the post-base64 bytes are zlib-compressed. Exercises the
// kittyExtractPayload step that runs Codec::Zlib::Inflater::decode before handing
// off to the f=32 pixel-store path.
/// hardReset reconstructs the Ubiquitty::Terminal in place and bumps the generation counter;
/// async-decode jobs snapshot it at submit and compare before touching Terminal.
/// Compressed direct-pixel payload whose inflated size disagrees with
/// the advertised width*height*bytesPerPixel. The async inflate path
/// must respond EINVAL:payload size mismatch and not install a slot.
/// Malformed zlib payload — the inflate worker hits Codec::Zlib::InflateStatus::Error and the
/// completion must report EINVAL. Exercises the worker's `status != Codec::Zlib::InflateStatus::Done`
/// arm together with the completion's `!j->success` branch.
/// A CHUNKED o=z transmit must drain its async inflate INLINE — like the
/// non-chunked path — so the image lands in the same processBytes() as the final
/// chunk, not one per-tick-drain later. A chunk-finalize path that returns
/// without drainAsyncDecodes costs a streamed-framebuffer client one frame of
/// latency per image, which reads as jitter rather than as a bug. Splits the
/// same 2x1 RGBA o=z payload as
/// testKittyGraphicsCompressedF32 across two chunks (m=1 continuation, m=0 final).
/// f=100 PNG payload — base64-wrapped 4×4 RGBA PNG with a deterministic
/// gradient. Exercises the full pipeline: base64 decode → IHDR parse →
/// IDAT inflate (dynamic-Huffman in this case) → filter-None unfilter →
/// pixel buffer landing on the TerminalImage.
/// A non-PNG f=100 payload (HEIF/AVIF/WebP/APNG container) routes through the
/// PAL image decoder — on macOS that is ImageIO. Here a stub stands in for
/// ImageIO: the kitty f=100 worker must detect the bytes are not a PNG
/// signature and hand them to the installed decoder, landing its RGBA on the
/// TerminalImage. Returns a distinctive 7×3 so a PNG-path fallthrough can't pass.
static int kittyStubCalls = 0;

static bool kittyContainerStubDecoder(std::span<const std::uint8_t> bytes, uint8_t** rgbaOut, int* widthOut, int* heightOut)
{
    (void)bytes;
    kittyStubCalls++;
    *rgbaOut = static_cast<uint8_t*>(malloc(7 * 3 * 4));
    if (!*rgbaOut)
        return false;
    memset(*rgbaOut, 0x5A, 7 * 3 * 4);
    *widthOut = 7;
    *heightOut = 3;
    return true;
}

TEST(testOSC1337InlineImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    stubDecodeCalls = 0;
    Ubiquitty::PAL::ScopedContext scopedImage(palContextWithImageDecoder(&stubImageDecoder));
    feed(t, "\033]1337;File=width=4;height=4;inline=1:QUFBQUFB\007");
    CHECK(stubDecodeCalls == 1);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(&t, 0);
    CHECK(img != nullptr && img->used && img->width == 4 && img->height == 4);
}

// The inline-image path decodes base64 through the same Codec the clipboard uses,
// and its payload is the part nothing else asserts. `QUFBQUFB` carries no `+` or
// `/`, so a decoder mapping either to the wrong sextet passes every other case
// here — the length is unchanged and the stub reports the image the escape's own
// parameters described.
//
// "+/+/" is the shortest payload that pins both characters: it decodes to
// FB FF BF, and a `/`-to-62 or `+`-to-63 defect moves those bytes without moving
// the count.
TEST(testOSC1337InlineImageDecodesTheWholeAlphabet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    stubDecodeCalls = 0;
    stubDecodeFirstBytes.clear();
    Ubiquitty::PAL::ScopedContext scopedImage(palContextWithImageDecoder(&stubImageDecoder));
    feed(t, "\033]1337;File=width=4;height=4;inline=1:+/+/\007");
    CHECK(stubDecodeCalls == 1);
    CHECK(stubDecodeFirstBytes.size() == 3);
    CHECK(stubDecodeFirstBytes[0] == 0xFB);
    CHECK(stubDecodeFirstBytes[1] == 0xFF);
    CHECK(stubDecodeFirstBytes[2] == 0xBF);
}

TEST(testOSC1337InlineImageNoDecoder)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Explicit no-decoder context (order-independent): OSC 1337 is consumed, no image.
    Ubiquitty::PAL::ScopedContext scopedNoDecoder(palContextWithImageDecoder(nullptr));
    feed(t, "\033]1337;File=inline=1:QUFBQUFB\007");
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
}

TEST(testKittyGraphicsQuietLevels)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    Ubiquitty::hardReset(t);

    // q absent (0): a successful 1x1 RGB transfer ACKs OK...
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=20;/wAA\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "OK") != nullptr);
    // ...and a failing one (no geometry) reports EINVAL.
    feed(t, "\033_Ga=T,t=d,f=24,i=21;/wAA\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "EINVAL") != nullptr);

    // q=1: success is silent...
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=22,q=1;/wAA\033\\");
    CHECK(drain(t, outputBuffer) == 0);
    // ...but a failure still reports: q=1 suppresses the success reply only, and
    // a client that never hears about a rejected transmission waits forever.
    feed(t, "\033_Ga=T,t=d,f=24,i=23,q=1;/wAA\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "EINVAL") != nullptr);

    // q=2: silent on success and failure.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=24,q=2;/wAA\033\\");
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033_Ga=T,t=d,f=24,i=25,q=2;/wAA\033\\");
    CHECK(drain(t, outputBuffer) == 0);
}

TEST(testKittyKeyboardProtocolStateAndQueries)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];

    Ubiquitty::hardReset(t);
    feed(t, "\033[>1u");
    CHECK(t.kittyKeyboardFlags[0] == std::to_underlying(Kitty::KeyFlag::Disambiguate));
    CHECK(t.kittyKeyboardStackLength[0] == 1);
    feed(t, "\033[=8;2u");
    CHECK(t.kittyKeyboardFlags[0] == Kitty::KeyFlagSet({ Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAll }).toRaw());
    feed(t, "\033[?u");
    CHECK(drain(t, outputBuffer) == 5);
    CHECK(strcmp(outputBuffer, "\033[?9u") == 0);
    feed(t, "\033[<u");
    CHECK(t.kittyKeyboardFlags[0] == 0);
    CHECK(t.kittyKeyboardStackLength[0] == 0);
}

TEST(testKittyKeyboardStacksArePerScreen)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033[>1u");
    feed(t, "\033[?1049h\033[>8u");
    CHECK(t.kittyKeyboardFlags[0] == std::to_underlying(Kitty::KeyFlag::Disambiguate));
    CHECK(t.kittyKeyboardFlags[1] == std::to_underlying(Kitty::KeyFlag::ReportAll));
    feed(t, "\033[?1049l");
    CHECK(t.kittyKeyboardFlags[0] == std::to_underlying(Kitty::KeyFlag::Disambiguate));
}

TEST(testKittyFlagsStackOverflowEvictsOldest)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Push 9 distinct flag sets: each push saves the current flags then sets new.
    // Saved sequence is 0,1,2,3,4,5,6,7,8; the 9th (saving 8) overflows the depth-8
    // stack and must evict the oldest (0), leaving [1,2,3,4,5,6,7,8].
    feed(t, "\033[>1u\033[>2u\033[>3u\033[>4u\033[>5u\033[>6u\033[>7u\033[>8u\033[>9u");
    // Pop 7, leaving one entry (so the empty-reset can't mask this). With eviction
    // the 7th restore is 2; dropping the newest entry instead would give 1.
    feed(t, "\033[<7u");
    CHECK(t.kittyKeyboardFlags[0] == 2);
}

TEST(testKittyFlagsStackPopEmptyResets)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[=5u"); // set flags = 5 with an empty stack
    CHECK(t.kittyKeyboardFlags[0] == 5);
    feed(t, "\033[>1u"); // push (saves 5), flags = 1
    feed(t, "\033[<u"); // pop empties the stack → flags reset to 0, not 5
    CHECK(t.kittyKeyboardFlags[0] == 0);
}

TEST(testKittyGraphicsDirectRGBDisplay)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=42,I=7,p=9;/wAA\033\\");
    img = findKittyImage(t, 42);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
        CHECK(img->imageNumber == 7);
        CHECK(img->width == 1);
        CHECK(img->height == 1);
        CHECK(img->format == 24);
        CHECK(img->pixelLength == 3);
        CHECK(img->pixels[0] == 0xFF);
        CHECK(img->pixels[1] == 0x00);
        CHECK(img->pixels[2] == 0x00);
    }
    // Where it is drawn, and under which placement ID, is the placement's.
    const Ubiquitty::Image::TerminalImagePlacement* placement = findKittyPlacement(t, 42, PlacementIDMatch::Require, 9);
    CHECK(placement != nullptr);
    if (placement) {
        CHECK(placement->cursorX == 0);
        CHECK(placement->cursorY == 0);
    }
    CHECK(cell(t, 0, 0)->codePoint == 0);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=42,p=9;OK") != nullptr);
}

TEST(testKittyExplicitIdPlacementScrollsWithText)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImagePlacement* p;

    Ubiquitty::hardReset(t);
    t.scrollbackLimit = 3; // small floor so the eviction leg stays fast + exact

    // Anchor an explicit-id placement at row 5 (0-indexed).
    feed(t, "\033[6;1H"); // CUP row 6 (1-indexed) -> cursorY 5
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=42;/wAA\033\\");

    p = findKittyPlacement(t, 42, PlacementIDMatch::Ignore, 0);
    CHECK(p != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    if (p)
        CHECK(p->cursorY == 5);
    // The condition a gate keyed on the allocator would miss: an explicit-id transmit leaves
    // the auto-id allocator at zero.
    CHECK(t.images.kittyNextImageID == 0);

    // Full-screen scroll up by 3 (LF at the bottom row).
    feed(t, "\033[24;1H");
    feed(t, "\n\n\n");

    p = findKittyPlacement(t, 42, PlacementIDMatch::Ignore, 0);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->cursorY == 2); // shifts to 2 with the text; a gate keyed on the allocator leaves it at 5

    // Keep scrolling until the placement passes the scrollback floor
    // (cursorY + rows <= -scrollbackLimit): it must then be evicted, exercising
    // the drop path (and its live-placement-count decrement).
    feed(t, "\033[24;1H");
    for (int i = 0; i < 12; i++)
        feed(t, "\n");
    CHECK(findKittyPlacement(t, 42, PlacementIDMatch::Ignore, 0) == nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
}

TEST(testKittyGraphicsDirectRgbaDisplay)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=32,s=1,v=1,i=43;/wAA/w==\033\\");
    img = findKittyImage(t, 43);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->format == 32);
        CHECK(img->pixelLength == 4);
        CHECK(img->pixels[0] == 0xFF);
        CHECK(img->pixels[3] == 0xFF);
    }
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testKittyImageMemoryBudget)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Tiny budget so a handful of small images trigger eviction without feeding
    // megabytes. Each image below is 16x16 RGB = 768 bytes; 2000 holds two.
    t.images.kittyImageBytesBudget = 2000;

    // 768 zero bytes encode as base64 "AAAA" x 256.
    char payload[1025];
    for (int i = 0; i < 1024; i += 4) {
        payload[i] = 'A';
        payload[i + 1] = 'A';
        payload[i + 2] = 'A';
        payload[i + 3] = 'A';
    }
    payload[1024] = '\0';

    // Eight transmit-only (a=t, non-displayed) images. Total would be 6144 bytes
    // if all kept; the budget evicts down to the two most-recent (1536 <= 2000).
    for (unsigned id = 1; id <= 8; ++id) {
        char header[64];
        snprintf(header, sizeof(header), "\033_Ga=t,t=d,f=24,s=16,v=16,i=%u;", id);
        feed(t, header);
        feed(t, payload);
        feed(t, "\033\\");
    }
    CHECK(Ubiquitty::Image::totalBytes(&t) <= t.images.kittyImageBytesBudget);
    CHECK(findKittyImage(t, 8) != nullptr); // two newest survive
    CHECK(findKittyImage(t, 7) != nullptr);
    CHECK(findKittyImage(t, 6) == nullptr); // older ones evicted
    CHECK(findKittyImage(t, 1) == nullptr);

    // A DISPLAYED image (a=T) is evicted only after non-displayed ones: transmit
    // it, then flood with transmit-only images — the displayed one outlives them.
    {
        char header[64];
        snprintf(header, sizeof(header), "\033_Ga=T,t=d,f=24,s=16,v=16,i=%u;", 100u);
        feed(t, header);
        feed(t, payload);
        feed(t, "\033\\");
    }
    for (unsigned id = 200; id <= 205; ++id) {
        char header[64];
        snprintf(header, sizeof(header), "\033_Ga=t,t=d,f=24,s=16,v=16,i=%u;", id);
        feed(t, header);
        feed(t, payload);
        feed(t, "\033\\");
    }
    CHECK(Ubiquitty::Image::totalBytes(&t) <= t.images.kittyImageBytesBudget);
    CHECK(findKittyImage(t, 100) != nullptr); // displayed → survived the flood
}

TEST(testKittyGraphicsChunkedPayload)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,i=77,m=1;AQID\033\\");
    CHECK(findKittyImage(t, 77) == nullptr);
    feed(t, "\033_Gm=0;BAUG\033\\");
    img = findKittyImage(t, 77);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->width == 2);
        CHECK(img->height == 1);
        CHECK(img->pixelLength == 6);
        CHECK(img->pixels[0] == 1);
        CHECK(img->pixels[5] == 6);
    }
}

TEST(testKittyGraphicsErrorResponseForBadPayload)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=88;@@@@\033\\");
    CHECK(findKittyImage(t, 88) == nullptr);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=88;EINVAL") != nullptr);
}

TEST(testKittyGraphicsUnsupportedTransportAndActionDoNotPaint)
{
    Ubiquitty::Terminal t;
    char outputBuffer[192];

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=f,f=24,s=1,v=1,i=90;HELLO\033\\");
    feed(t, "\033_Ga=z,t=d,f=24,s=1,v=1,i=91;HELLO\033\\");
    CHECK(findKittyImage(t, 90) == nullptr);
    CHECK(findKittyImage(t, 91) == nullptr);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 0, 1)->codePoint == 0);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "ENOTSUP") != nullptr);
}

// Named images request acknowledgements. Explicit queries also require a
// reply, even without an image identifier; anonymous transmissions stay silent.
TEST(testKittyGraphicsRepliesToNamedImagesAndExplicitQueries)
{
    Ubiquitty::Terminal t;
    char outputBuffer[192];

    Ubiquitty::hardReset(t);
    feed(t, "\033_Gf=24,s=1,v=1;AAAA\033\\");
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033_Ga=q,f=24,s=2,v=2;AAAA\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "EINVAL") != nullptr);
    feed(t, "\033_Gf=24,s=1,v=1,i=56;AAAA\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "i=56;OK") != nullptr);
}

TEST(testKittyGraphicsDeleteAndQuery)
{
    Ubiquitty::Terminal t;
    char outputBuffer[192];

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=55,q=1;/wAA\033\\");
    CHECK(findKittyImage(t, 55) != nullptr);
    feed(t, "\033_Ga=q,i=55;\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=55;OK") != nullptr);
    feed(t, "\033_Ga=d,d=I,i=55;\033\\");
    CHECK(findKittyImage(t, 55) == nullptr);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=55;OK") != nullptr);
    feed(t, "\033_Ga=q,i=55;\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=55;ENOENT") != nullptr);
}

TEST(testKittyGraphicsNonGraphicsAPCRawBytesDoNotPaint)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\033_HELLO RAW APC\033\\Z");
    CHECK(cell(t, 0, 0)->codePoint == 'Z');
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testKittyGraphicsFormat24Vs32PixelDepthDiffers)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* rgb;
    const Ubiquitty::Image::TerminalImage* rgba;

    Ubiquitty::hardReset(t);

    // 2x1 RGB image: 6 bytes — red, green pixels.
    // Base64("\xFF\x00\x00\x00\xFF\x00") = "/wAAAP8A"
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,i=2401;/wAAAP8A\033\\");
    rgb = findKittyImage(t, 2401);
    CHECK(rgb != nullptr);
    if (rgb) {
        CHECK(rgb->format == 24);
        CHECK(rgb->width == 2);
        CHECK(rgb->height == 1);
        // 24-bit packed: 3 bytes per pixel, no alpha.
        CHECK(rgb->pixelLength == 6);
        CHECK(rgb->pixels[0] == 0xFF); // R0
        CHECK(rgb->pixels[1] == 0x00); // G0
        CHECK(rgb->pixels[2] == 0x00); // B0
        CHECK(rgb->pixels[3] == 0x00); // R1
        CHECK(rgb->pixels[4] == 0xFF); // G1
        CHECK(rgb->pixels[5] == 0x00); // B1
    }

    // 2x1 RGBA image: 8 bytes — red opaque, green half-transparent.
    // Base64("\xFF\x00\x00\xFF\x00\xFF\x00\x80") = "/wAA/wD/AIA="
    feed(t, "\033_Ga=T,t=d,f=32,s=2,v=1,i=3201;/wAA/wD/AIA=\033\\");
    rgba = findKittyImage(t, 3201);
    CHECK(rgba != nullptr);
    if (rgba) {
        CHECK(rgba->format == 32);
        CHECK(rgba->width == 2);
        CHECK(rgba->height == 1);
        // 32-bit aligned: 4 bytes per pixel, alpha preserved verbatim.
        CHECK(rgba->pixelLength == 8);
        CHECK(rgba->pixels[0] == 0xFF); // R0
        CHECK(rgba->pixels[1] == 0x00); // G0
        CHECK(rgba->pixels[2] == 0x00); // B0
        CHECK(rgba->pixels[3] == 0xFF); // A0 — fully opaque
        CHECK(rgba->pixels[4] == 0x00); // R1
        CHECK(rgba->pixels[5] == 0xFF); // G1
        CHECK(rgba->pixels[6] == 0x00); // B1
        CHECK(rgba->pixels[7] == 0x80); // A1 — half-transparent
    }

    // The two formats land in independent slots and don't alias bytes.
    if (rgb && rgba) {
        CHECK(rgb->pixelLength != rgba->pixelLength);
        CHECK(rgb->pixels != rgba->pixels);
    }
}

TEST(testKittyGraphicsPNGF100)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    /// 4×4 RGBA PNG, hand-crafted via Python's zlib. Filter type 0 on
    /// every row, so the test exercises inflate + chunk dispatch without
    /// also stressing the Sub/Up/Avg/Paeth code paths.
    static const char png[] = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAANklEQVR4"
                              "2g3IQQEAMAwCMaRUClIqBSmTgrNb84wkYQ2RqRbJg23ipc5FjLMkoXkX"
                              "XdyQPtryAdXpIfGK7hesAAAAAElFTkSuQmCC";
    char buffer[256];
    int n;

    Ubiquitty::hardReset(t);
    n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=d,f=100,i=4242;%s\033\\", png);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    img = findKittyImage(t, 4242);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
        CHECK(img->format == 32); // PNG colour type 6 (RGBA) → 32
        CHECK(img->width == 4);
        CHECK(img->height == 4);
        CHECK(img->pixelLength == 4 * 4 * 4);
        // Top-left pixel encodes (0,0,0,255).
        CHECK(img->pixels[0] == 0x00);
        CHECK(img->pixels[1] == 0x00);
        CHECK(img->pixels[2] == 0x00);
        CHECK(img->pixels[3] == 0xFF);
        // (1,0) = (64, 0, 32, 255).
        CHECK(img->pixels[4] == 64);
        CHECK(img->pixels[5] == 0);
        CHECK(img->pixels[6] == 32);
        CHECK(img->pixels[7] == 0xFF);
        // Bottom-right pixel (3,3) = (192, 192, 192, 255).
        // Offset = (3 * 4 + 3) * 4 = 60.
        CHECK(img->pixels[60] == 192);
        CHECK(img->pixels[61] == 192);
        CHECK(img->pixels[62] == 192);
        CHECK(img->pixels[63] == 0xFF);
    }
}

TEST(testKittyGraphicsContainerF100ViaPlatform)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    kittyStubCalls = 0;
    Ubiquitty::PAL::ScopedContext scopedImage(palContextWithImageDecoder(&kittyContainerStubDecoder));
    // base64("RIFF....WEBP") would be a real WebP; here any non-PNG bytes do —
    // "QUFBQUFBQUFB" = "AAAAAAAAAA", which lacks the 0x89 PNG magic, so the
    // f=100 worker routes it to the PAL decoder.
    feed(t, "\033_Ga=T,t=d,f=100,i=7373;QUFBQUFBQUFB\033\\");
    CHECK(kittyStubCalls == 1);
    img = findKittyImage(t, 7373);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
        CHECK(img->format == 32); // PAL decoder always yields RGBA
        CHECK(img->width == 7);
        CHECK(img->height == 3);
        CHECK(img->pixelLength == 7 * 3 * 4);
    }
}

TEST(testKittyGraphicsCompressedF32)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    /// zlib(level=9) of {0xFF,0x00,0x00,0xFF, 0x00,0xFF,0x00,0x80}, then
    /// base64. Generated via Python's zlib + base64.
    static const char compressed[] = "eNr7z8DwHwgbABB4A34=";
    char buffer[160];
    int n;

    Ubiquitty::hardReset(t);
    n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=d,f=32,o=z,s=2,v=1,i=5151;%s\033\\", compressed);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    img = findKittyImage(t, 5151);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->format == 32);
        CHECK(img->width == 2);
        CHECK(img->height == 1);
        CHECK(img->pixelLength == 8);
        CHECK(img->pixels[0] == 0xFF);
        CHECK(img->pixels[1] == 0x00);
        CHECK(img->pixels[2] == 0x00);
        CHECK(img->pixels[3] == 0xFF);
        CHECK(img->pixels[4] == 0x00);
        CHECK(img->pixels[5] == 0xFF);
        CHECK(img->pixels[6] == 0x00);
        CHECK(img->pixels[7] == 0x80);
    }
}

TEST(testKittyGraphicsChunkedOzDrainsInline)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // "eNr7z8DwHwgbABB4A34=" split on a 4-char base64 boundary across two chunks.
    feed(t, "\033_Ga=T,t=d,f=32,o=z,s=2,v=1,i=7373,m=1;eNr7z8Dw\033\\");
    feed(t, "\033_Gm=0;HwgbABB4A34=\033\\");
    /// Inline drain: the inflated frame is installed + placed in this same feed(),
    /// not deferred to the next tick (what the missing chunk-path drain caused).
    const Ubiquitty::Image::TerminalImage* img = findKittyImage(t, 7373);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->format == 32);
        CHECK(img->width == 2 && img->height == 1);
        CHECK(img->pixelLength == 8);
        CHECK(img->pixels[0] == 0xFF && img->pixels[3] == 0xFF); // red, opaque
    }
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
}

TEST(testKittyGraphicsCompressedSizeMismatch)
{
    Ubiquitty::Terminal t;
    char buffer[160];
    char outputBuffer[256];
    /// Same compressed 8-byte payload as the happy-path test below,
    /// but we ASK for s=2,v=2 (= 16 bytes expected) which mismatches.
    static const char compressed[] = "eNr7z8DwHwgbABB4A34=";
    Ubiquitty::hardReset(t);
    int n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=d,f=32,o=z,s=2,v=2,i=7171;%s\033\\", compressed);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    // No slot installed:
    CHECK(findKittyImage(t, 7171) == nullptr);
    // And the response carries the EINVAL:
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "EINVAL:payload size mismatch") != nullptr);
}

TEST(testKittyGraphicsCompressedBadZlib)
{
    Ubiquitty::Terminal t;
    char buffer[160];
    char outputBuffer[256];
    /// Garbage bytes — won't parse as zlib header.
    static const char badCompressed[] = "QUJDRA=="; // base64 of "ABCD"
    Ubiquitty::hardReset(t);
    int n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=d,f=32,o=z,s=2,v=1,i=7272;%s\033\\", badCompressed);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    CHECK(findKittyImage(t, 7272) == nullptr);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "EINVAL") != nullptr);
}

TEST(testKittyGraphicsTFDispatches)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    /// Raw RGB bytes: 1 red pixel.
    static const unsigned char file[] = { 0xFF, 0x00, 0x00 };
    /// base64("/tmp/ubiquitty-test") = "L3RtcC91YmlxdWl0dHktdGVzdA==".
    static const char path[] = "/tmp/ubiquitty-test";
    static const char pathB64[] = "L3RtcC91YmlxdWl0dHktdGVzdA==";
    char buffer[160];
    int n;
    Ubiquitty::hardReset(t);
    installFakeFileSystem(t, path, file, static_cast<int>(sizeof(file)));
    n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=f,f=24,s=1,v=1,i=2010;%s\033\\", pathB64);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    img = findKittyImage(t, 2010);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->format == 24);
        CHECK(img->width == 1);
        CHECK(img->height == 1);
        CHECK(img->pixelLength == 3);
        CHECK(img->pixels[0] == 0xFF);
        CHECK(img->pixels[1] == 0x00);
        CHECK(img->pixels[2] == 0x00);
    }
    CHECK(fakeFileSystemUnlinkCount == 0);
}

TEST(testKittyGraphicsTSDispatches)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    /// Raw RGB bytes: 1 green pixel.
    static const unsigned char object[] = { 0x00, 0xFF, 0x00 };
    /// base64("/ubq-shm") = "L3VicS1zaG0=".
    static const char name[] = "/ubq-shm";
    static const char nameB64[] = "L3VicS1zaG0=";
    char buffer[160];
    int n;
    Ubiquitty::hardReset(t);
    installFakeFileSystem(t, name, object, static_cast<int>(sizeof(object)));
    n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=s,f=24,s=1,v=1,S=3,i=2011;%s\033\\", nameB64);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    img = findKittyImage(t, 2011);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->format == 24);
        CHECK(img->width == 1);
        CHECK(img->height == 1);
        CHECK(img->pixelLength == 3);
        CHECK(img->pixels[0] == 0x00);
        CHECK(img->pixels[1] == 0xFF);
        CHECK(img->pixels[2] == 0x00);
    }
    CHECK(fakeFileSystemUnlinkCount == 1); // t=s consumes: object unlinked after the read
}

TEST(testKittyTFGatedByCapabilityPolicy)
{
    static const unsigned char file[] = { 0xFF, 0x00, 0x00 };
    static const char path[] = "/tmp/ubiquitty-test";
    static const char pathB64[] = "L3RtcC91YmlxdWl0dHktdGVzdA==";
    char buffer[160];
    char outputBuffer[160];
    int n;

    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, path, file, static_cast<int>(sizeof(file)));
        fakeCapabilityPolicy.wasCalled = false;
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=f,f=24,s=1,v=1,i=2020;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        CHECK(fakeCapabilityPolicy.wasCalled);
        // The host file read is gated on FileReadAccess. Assert it specifically
        // (not lastCapability) because the EPERM ACK below now also consults the
        // policy as Capability::None, moving lastCapability past the file read.
        CHECK(fakeCapabilityPolicy.sawFileRead);
        // Origin axis: the gate reads the Terminal's origin (default Local for a
        // direct local frontend), not a hardcoded value.
        CHECK(fakeCapabilityPolicy.fileReadOrigin == Ubiquitty::CapabilityOrigin::Local);
        CHECK(findKittyImage(t, 2020) == nullptr); // denied → no read
        // Deny goes through the Kitty error channel (EPERM), not a silent drop,
        // so a conforming client re-transmits after the grant. The EPERM ACK is a
        // pure-protocol-ACK (Capability::None ⇒ always-allow), so it is emitted
        // even though the FileReadAccess read was denied.
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=2020;EPERM") != nullptr);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, path, file, static_cast<int>(sizeof(file)));
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        // A stamped non-Local origin (an agent-driven session) threads
        // through to the gate — the host keys its verdict on (Capability × Origin).
        t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Agent;
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=f,f=24,s=1,v=1,i=2021;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        CHECK(fakeCapabilityPolicy.lastOrigin == Ubiquitty::CapabilityOrigin::Agent);
        CHECK(findKittyImage(t, 2021) != nullptr); // allowed → image installed
    }
    {
        /// The not-a-silent-drop contract under the REAL tier table at the
        /// least-trusted principal: a Remote-origin t=f read is DENIED
        /// (FileReadAccess→HostEscaping→Remote=Deny), so the file is never read —
        /// but the EPERM ACK is still EMITTED, because that ACK is a pure protocol
        /// reply (Capability::None→Benign→Remote=Allow). A conforming client reads
        /// EPERM and re-transmits after the human grants the capability; the read
        /// is gated, the error channel is not. Drives the production
        /// HostCapabilityPolicy, not the fake, so the tier table itself is pinned.
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, path, file, static_cast<int>(sizeof(file)));
        Ubiquitty::HostCapabilityPolicy realPolicy; // no grant store ⇒ pure tier table
        t.capabilityPolicy = &realPolicy;
        t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=f,f=24,s=1,v=1,i=2022;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        CHECK(findKittyImage(t, 2022) == nullptr); // Remote read denied → file unread
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=2022;EPERM") != nullptr); // but the EPERM ACK is emitted
        t.capabilityPolicy = nullptr; // clear the CheckedPtr before realPolicy destructs
    }
}

TEST(testKittyGraphicsQueryValidatesLoad)
{
    static const unsigned char redPixel[] = { 0xFF, 0x00, 0x00 };
    static const char path[] = "/tmp/ubiquitty-test";
    static const char pathB64[] = "L3RtcC91YmlxdWl0dHktdGVzdA==";
    char buffer[192];
    char outputBuffer[192];
    int n;

    // (1) Direct payload, correct size (1x1 RGB): OK, and nothing stored (fresh id).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033_Ga=q,f=24,s=1,v=1,i=7001;/wAA\033\\");
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7001;OK") != nullptr);
        CHECK(findKittyImage(t, 7001) == nullptr);
    }
    // (2) Negative control: a size MISMATCH now fails — the medium/format is exercised,
    //     not just id existence. s=2,v=1 needs 6 bytes; /wAA is 3.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033_Ga=q,f=24,s=2,v=1,i=7002;/wAA\033\\");
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7002;EINVAL") != nullptr);
        CHECK(strstr(outputBuffer, "i=7002;OK") == nullptr);
    }
    // (3) A bad o=z compression stream → decode error, not OK.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033_Ga=q,f=24,s=1,v=1,o=z,i=7003;/wAA\033\\");
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7003;OK") == nullptr);
    }
    // (4) t=f without a FileReadClient → ENOTSUP (the medium is validated).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=q,t=f,f=24,s=1,v=1,i=7004;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7004;ENOTSUP") != nullptr);
    }
    // (5) t=f with a fake file → OK, and nothing stored (the probe reads then discards).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, path, redPixel, static_cast<int>(sizeof(redPixel)));
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=q,t=f,f=24,s=1,v=1,i=7005;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7005;OK") != nullptr);
        CHECK(findKittyImage(t, 7005) == nullptr);
    }
    // (6) The FileReadAccess gate still applies to a=q file probes → EPERM when denied
    //     (regression-locks t4: a query must not become an ungated arbitrary-file read).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, path, redPixel, static_cast<int>(sizeof(redPixel)));
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=q,t=f,f=24,s=1,v=1,i=7009;%s\033\\", pathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7009;EPERM") != nullptr);
        CHECK(findKittyImage(t, 7009) == nullptr);
    }
    // (7) A t=t (temp) QUERY must not unlink the file; a real a=t transmit does — regression-
    //     locks t5 and proves the TempFileUnlink Preserve-vs-Consume wiring. The name
    //     carries the protocol's t=t marker, without which neither would be read.
    {
        static const char temporaryPath[] = "/tmp/tty-graphics-protocol-probe";
        static const char temporaryPathB64[] = "L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wtcHJvYmU=";
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, temporaryPath, redPixel, static_cast<int>(sizeof(redPixel)));
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=q,t=t,f=24,s=1,v=1,i=7006;%s\033\\", temporaryPathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7006;OK") != nullptr);
        CHECK(fakeFileSystemUnlinkCount == 0); // the probe left the temp file intact
        CHECK(findKittyImage(t, 7006) == nullptr);
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=t,t=t,f=24,s=1,v=1,i=7007;%s\033\\", temporaryPathB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(fakeFileSystemUnlinkCount == 1); // a real transmit consumed it
    }
    // (7b) The same Preserve-vs-Consume invariant for t=s: a QUERY must never
    //     shm_unlink the client's object (a regression hardcoding the unlink
    //     would turn a probe into consuming it — the t=s threat model's named
    //     invariant); a real transmit consumes it. Mirrors (7).
    {
        static const char shmName[] = "/ubq-shm";
        static const char shmNameB64[] = "L3VicS1zaG0=";
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        installFakeFileSystem(t, shmName, redPixel, static_cast<int>(sizeof(redPixel)));
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=q,t=s,f=24,s=1,v=1,S=3,i=7010;%s\033\\", shmNameB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7010;OK") != nullptr);
        CHECK(fakeFileSystemUnlinkCount == 0); // the probe left the shm object intact
        CHECK(findKittyImage(t, 7010) == nullptr);
        n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=s,f=24,s=1,v=1,S=3,i=7011;%s\033\\", shmNameB64);
        CHECK(n > 0 && n < (int)sizeof(buffer));
        feed(t, buffer);
        drain(t, outputBuffer);
        CHECK(fakeFileSystemUnlinkCount == 1); // a real transmit consumed the object
    }
    // (8) A degenerate no-payload query keeps id-existence semantics.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7008,q=1;/wAA\033\\");
        CHECK(findKittyImage(t, 7008) != nullptr);
        feed(t, "\033_Ga=q,i=7008;\033\\");
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=7008;OK") != nullptr);
        feed(t, "\033_Ga=q,i=9999;\033\\");
        drain(t, outputBuffer);
        CHECK(strstr(outputBuffer, "i=9999;ENOENT") != nullptr);
    }
}

TEST(testKittyGraphicsTTUnlinksTemp)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    static const unsigned char file[] = { 0x00, 0xFF, 0x00 };
    static const char path[] = "/tmp/tty-graphics-protocol-tt";
    static const char pathB64[] = "L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wtdHQ=";
    char buffer[160];
    int n;
    Ubiquitty::hardReset(t);
    installFakeFileSystem(t, path, file, static_cast<int>(sizeof(file)));
    n = snprintf(buffer, sizeof(buffer), "\033_Ga=T,t=t,f=24,s=1,v=1,i=2020;%s\033\\", pathB64);
    CHECK(n > 0 && n < (int)sizeof(buffer));
    feed(t, buffer);
    img = findKittyImage(t, 2020);
    CHECK(img != nullptr);
    CHECK(fakeFileSystemUnlinkCount == 1);
}

TEST(testKittyGraphicsTSReturnsError)
{
    Ubiquitty::Terminal t;
    char outputBuffer[192];
    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=s,f=24,s=1,v=1,i=2030;AAAAAA==\033\\");
    CHECK(findKittyImage(t, 2030) == nullptr);
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=2030;ENOTSUP") != nullptr);
}

TEST(test_kitty_graphics_placement_pixel_offsets_X_Y)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    const Ubiquitty::Image::TerminalImage* img;
    const Ubiquitty::Image::TerminalImagePlacement* p;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=1010,p=1,X=10,Y=5;/wAA\033\\");
    img = findKittyImage(t, 1010);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
    }
    p = findKittyPlacement(t, 1010, PlacementIDMatch::Require, 1);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->pixelOffsetX == 10);
        CHECK(p->pixelOffsetY == 5);
        CHECK(p->placementID == 1);
        CHECK(p->hasPlacementID);
    }
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=1010,p=1;OK") != nullptr);
}

TEST(testKittyGraphicsPlacementCellScalingCR)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    const Ubiquitty::Image::TerminalImage* img;
    const Ubiquitty::Image::TerminalImagePlacement* p;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=2020,p=2,c=20,r=10;/wAA\033\\");
    img = findKittyImage(t, 2020);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
        // Pixel dimensions must still be the source size — c/r is a
        // render-time hint, not a resize of the stored pixels.
        CHECK(img->width == 1);
        CHECK(img->height == 1);
    }
    p = findKittyPlacement(t, 2020, PlacementIDMatch::Require, 2);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->cellColumns == 20);
        CHECK(p->cellRows == 10);
    }
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=2020,p=2;OK") != nullptr);
}

TEST(testKittyGraphicsPlacementZIndex)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    const Ubiquitty::Image::TerminalImage* below;
    const Ubiquitty::Image::TerminalImage* above;
    const Ubiquitty::Image::TerminalImagePlacement* pBelow;
    const Ubiquitty::Image::TerminalImagePlacement* pAbove;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=3001,p=11,z=0;/wAA\033\\");
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=3002,p=12,z=5;AAD/\033\\");

    below = findKittyImage(t, 3001);
    above = findKittyImage(t, 3002);
    CHECK(below != nullptr);
    CHECK(above != nullptr);

    pBelow = findKittyPlacement(t, 3001, PlacementIDMatch::Require, 11);
    pAbove = findKittyPlacement(t, 3002, PlacementIDMatch::Require, 12);
    CHECK(pBelow != nullptr);
    CHECK(pAbove != nullptr);
    if (pBelow)
        CHECK(pBelow->zIndex == 0);
    if (pAbove)
        CHECK(pAbove->zIndex == 5);

    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=3001,p=11;OK") != nullptr);
    CHECK(strstr(outputBuffer, "i=3002,p=12;OK") != nullptr);
}

TEST(testKittyGraphicsSourceRectangle)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    const Ubiquitty::Image::TerminalImage* img;
    const Ubiquitty::Image::TerminalImagePlacement* p;

    Ubiquitty::hardReset(t);
    // Store a 4x4 RGB image (48 bytes = 64 chars base64 of zeros).
    feed(t, "\033_Ga=T,t=d,f=24,s=4,v=4,i=4040,p=3,x=10,y=5,w=20,h=15;AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\033\\");
    img = findKittyImage(t, 4040);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->displayed);
        CHECK(img->width == 4);
        CHECK(img->height == 4);
    }
    p = findKittyPlacement(t, 4040, PlacementIDMatch::Require, 3);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->sourceX == 10);
        CHECK(p->sourceY == 5);
        CHECK(p->sourceWidth == 20);
        CHECK(p->sourceHeight == 15);
    }
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "i=4040,p=3;OK") != nullptr);
}

TEST(testKittyGraphicsPlacementMultiplePerImage)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int i;
    int matches = 0;
    bool sawP1 = false;
    bool sawP2 = false;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=5050,p=1,X=10,Y=5;/wAA\033\\");
    // a=p (display existing) — new placement, same image, new id.
    // The empty payload still needs the `;` separator.
    feed(t, "\033_Ga=p,i=5050,p=2,X=20,Y=15;\033\\");
    CHECK(findKittyImage(t, 5050) != nullptr);
    for (i = 0; i < Ubiquitty::Image::maxTerminalImagePlacementCount; i++) {
        const Ubiquitty::Image::TerminalImagePlacement* p = Ubiquitty::Image::placement(&t, i);
        if (!p || p->imageID != 5050)
            continue;
        matches++;
        if (p->placementID == 1 && p->pixelOffsetX == 10 && p->pixelOffsetY == 5)
            sawP1 = true;
        if (p->placementID == 2 && p->pixelOffsetX == 20 && p->pixelOffsetY == 15)
            sawP2 = true;
    }
    CHECK(matches == 2);
    CHECK(sawP1);
    CHECK(sawP2);
    drain(t, outputBuffer);
}

TEST(test_kitty_graphics_placement_cursor_policy_C)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImagePlacement* p;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=6060,p=1,C=1;/wAA\033\\");
    p = findKittyPlacement(t, 6060, PlacementIDMatch::Require, 1);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->cursorPolicy == 1);

    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=6061,p=1,C=0;/wAA\033\\");
    p = findKittyPlacement(t, 6061, PlacementIDMatch::Require, 1);
    CHECK(p != nullptr);
    if (p)
        CHECK(p->cursorPolicy == 0);
}

TEST(test_kitty_graphics_cursor_advances_after_T)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Image #1: 1x1 image, no c/r — one cell, so the cursor stays on its row
    // and steps one column right of it, as kitty's own cursor does.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7100;/wAA\033\\");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 1);

    // Image #2: c=3,r=2 — lands on the image's last row (1 below the top),
    // right of its last column.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7101,c=3,r=2;/wAA\033\\");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 4);

    // Image #3: C=1 (stay) — no advance.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7102,C=1,r=4;/wAA\033\\");
    CHECK(t.screens.active->cursorY == 1);

    // Image #4: r=5 (default cursor policy C=0) — advances by r-1 = 4.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7103,r=5;/wAA\033\\");
    CHECK(t.screens.active->cursorY == 5);

    // A box stated past any grid: c and r at the parser's saturation ceiling
    // must not carry through to cursorX + columns as a signed overflow. A
    // non-zero starting column makes the addition overflow without the clamp,
    // which a sanitiser build traps; the advance clamps to the grid either
    // way, so the cursor lands in bounds.
    Ubiquitty::hardReset(t);
    feed(t, "X"); // cursorX = 1, so the advance adds to a non-zero column
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7104,c=9999999999,r=9999999999;/wAA\033\\");
    CHECK(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < t.columns);
    CHECK(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < t.rows);
}

TEST(testKittyGraphicsAnimationAddsFrames)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    // Base image (red 1x1).
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7001;/wAA\033\\");
    img = findKittyImage(t, 7001);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->frameCount == 1);
        // Frame 0 aliases the base pixel buffer.
        CHECK(img->frames[0].used);
        CHECK(img->frames[0].pixels == img->pixels);
    }

    // Frame 1 (green) with 100 ms delay.
    feed(t, "\033_Ga=f,i=7001,f=24,s=1,v=1,z=100;AP8A\033\\");
    // Frame 2 (blue) with 250 ms delay.
    feed(t, "\033_Ga=f,i=7001,f=24,s=1,v=1,z=250;AAD/\033\\");

    img = findKittyImage(t, 7001);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->frameCount == 3);
        CHECK(img->frames[1].used);
        CHECK(img->frames[1].pixelLength == 3);
        CHECK(img->frames[1].delayMilliseconds == 100);
        CHECK(img->frames[1].pixels[0] == 0x00);
        CHECK(img->frames[1].pixels[1] == 0xFF);
        CHECK(img->frames[1].pixels[2] == 0x00);
        CHECK(img->frames[2].used);
        CHECK(img->frames[2].pixelLength == 3);
        CHECK(img->frames[2].delayMilliseconds == 250);
        CHECK(img->frames[2].pixels[0] == 0x00);
        CHECK(img->frames[2].pixels[1] == 0x00);
        CHECK(img->frames[2].pixels[2] == 0xFF);
        // Animation default is stopped; an explicit a=a is required to
        // start playback.
        CHECK(img->animationState == Ubiquitty::Image::AnimationState::Stopped);
        CHECK(img->currentFrame == 0);
    }
}

// A looping animation returns to the same frame with the same clock after one
// full cycle, so the tick may drop whole cycles before it walks. That is an
// equivalence claim, not an approximation, and it is held by walking the same
// total in single milliseconds and comparing where the two land.
//
// It matters because the walk takes one step per frame boundary: with the 1 ms
// gap a sender can set, the step count is the elapsed milliseconds, and the
// elapsed milliseconds are whatever a clock idle through a backgrounded app or
// a monotonic wrap reports.
TEST(testKittyAnimationTickDropsWholeCyclesExactly)
{
    // Three frames with unlike gaps, so a cycle is 40 + 10 + 30 = 80 ms and the
    // phase within it is distinguishable at every offset.
    const auto build = [](Ubiquitty::Terminal& term) {
        Ubiquitty::hardReset(term);
        feed(term, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7100;/wAA\033\\");
        feed(term, "\033_Ga=f,i=7100,f=24,s=1,v=1,z=10;AP8A\033\\");
        feed(term, "\033_Ga=f,i=7100,f=24,s=1,v=1,z=30;AAD/\033\\");
        feed(term, "\033_Ga=a,i=7100,c=3;\033\\"); // c=3 = loop
    };

    int comparisons = 0;
    int framesReached = 0;
    for (uint32_t elapsed : { 1u, 39u, 40u, 41u, 79u, 80u, 81u, 160u, 247u, 4000u, 100000u, 4200000u }) {
        Ubiquitty::Terminal dropped;
        Ubiquitty::Terminal walked;
        build(dropped);
        build(walked);

        Ubiquitty::Image::advanceFrames(&dropped, elapsed);
        // The same total, a millisecond at a time, which cannot drop a cycle.
        for (uint32_t step = 0; step < elapsed; ++step)
            Ubiquitty::Image::advanceFrames(&walked, 1);

        const Ubiquitty::Image::TerminalImage* a = findKittyImage(dropped, 7100);
        const Ubiquitty::Image::TerminalImage* b = findKittyImage(walked, 7100);
        CHECK(a != nullptr && b != nullptr);
        if (a && b) {
            CHECK_MSG(a->currentFrame == b->currentFrame, "the two ticks landed on different frames");
            CHECK_MSG(a->frameClockMilliseconds == b->frameClockMilliseconds, "the two ticks landed at different phases");
            CHECK_MSG(a->animationState == b->animationState, "the two ticks left different animation states");
            if (a->currentFrame != 0)
                ++framesReached;
        }
        ++comparisons;
    }
    CHECK_EQ(comparisons, 12);
    // Guards a corpus that never leaves the first frame, where any tick agrees
    // with any other.
    CHECK_MSG(framesReached > 0, "no offset advanced past the first frame");
}

TEST(testKittyAnimationRunOnceStopsAtTheLastFrameHoweverLongTheGap)
{
    // The bound above is for a looping animation. A run-once one stops at its
    // last frame, so a gap of any size must leave it there rather than walking
    // past it or wrapping.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7101;/wAA\033\\");
    feed(t, "\033_Ga=f,i=7101,f=24,s=1,v=1,z=10;AP8A\033\\");
    feed(t, "\033_Ga=a,i=7101,c=2;\033\\"); // c=2 = run once

    Ubiquitty::Image::advanceFrames(&t, 4200000);
    const Ubiquitty::Image::TerminalImage* img = findKittyImage(t, 7101);
    CHECK(img != nullptr);
    if (img) {
        CHECK_MSG(img->currentFrame == img->frameCount - 1, "a run-once animation left its last frame");
        CHECK_MSG(img->animationState == Ubiquitty::Image::AnimationState::Stopped, "a run-once animation did not stop");
    }
}

TEST(testKittyAnimationFrameNumberPastSixteenBitsNamesNoFrame)
{
    // r is the frame to show, counted from 1, and a two-frame image has two.
    // A number that narrows to a valid one, 65536 above all, which narrows to
    // 0 and then to frame 65535, must leave the frame where it was: the tick
    // reads frames[currentFrame] from a fixed array.
    for (const char* frame : { "65536", "65537", "131072", "2147483647" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7103;/wAA\033\\");
        feed(t, "\033_Ga=f,i=7103,f=24,s=1,v=1,z=10;AP8A\033\\");
        feed(t, std::string("\033_Ga=a,i=7103,c=3,r=") + frame + "\033\\");
        const Ubiquitty::Image::TerminalImage* img = findKittyImage(t, 7103);
        CHECK(img != nullptr);
        if (!img)
            continue;
        CHECK_MSG(img->currentFrame == 0, "an out-of-range frame number moved the animation");
        if (img->currentFrame >= img->frameCount)
            continue; // the tick below would read past the frames
        Ubiquitty::Image::advanceFrames(&t, 100);
        img = findKittyImage(t, 7103);
        CHECK(img && img->currentFrame < img->frameCount);
    }
}

TEST(testKittyGraphicsAnimationControlAndTick)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=7002;/wAA\033\\");
    feed(t, "\033_Ga=f,i=7002,f=24,s=1,v=1,z=100;AP8A\033\\");
    feed(t, "\033_Ga=f,i=7002,f=24,s=1,v=1,z=250;AAD/\033\\");

    // c=2 = run-once.
    feed(t, "\033_Ga=a,i=7002,c=2;\033\\");
    img = findKittyImage(t, 7002);
    CHECK(img != nullptr);
    if (img)
        CHECK(img->animationState == Ubiquitty::Image::AnimationState::RunOnce);

    /// Frame 0 (the base/root frame) has no explicit gap. Per the kitty
    /// spec a 0/unspecified gap means the DEFAULT gap (40ms) — the root
    /// frame is a real, visible frame 1, not instantly skipped. Advance
    /// 30 ms — still on frame 0.
    Ubiquitty::Image::advanceFrames(&t, 30);
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->currentFrame == 0);
        CHECK(img->frameClockMilliseconds == 30);
    }

    /// Advance another 10 ms (total exactly 40ms default) — jump to frame 1
    /// with a fresh clock.
    Ubiquitty::Image::advanceFrames(&t, 10);
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->currentFrame == 1);
        CHECK(img->frameClockMilliseconds == 0);
    }

    /// Now sit on frame 1 (delay 100ms). Advance 50 ms — still frame 1.
    Ubiquitty::Image::advanceFrames(&t, 50);
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->currentFrame == 1);
        CHECK(img->frameClockMilliseconds == 50);
    }

    /// Advance another 60 ms (total 110 on frame 1) — wraps to frame 2.
    Ubiquitty::Image::advanceFrames(&t, 60);
    img = findKittyImage(t, 7002);
    if (img)
        CHECK(img->currentFrame == 2);

    /// Frame 2 delay is 250ms. Advance 300 — run-once stops on frame 2.
    Ubiquitty::Image::advanceFrames(&t, 300);
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->currentFrame == 2);
        CHECK(img->animationState == Ubiquitty::Image::AnimationState::Stopped);
    }

    // Now start looping; expect it to wrap to frame 0 when frame 2's
    // delay elapses again, and SHOW frame 0 for the 40ms default gap.
    feed(t, "\033_Ga=a,i=7002,c=3;\033\\");
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->animationState == Ubiquitty::Image::AnimationState::Looping);
        // Currently on frame 2.
        CHECK(img->currentFrame == 2);
    }
    /// 250 ms on frame 2 → wrap to frame 0. The remaining 10ms is < the
    /// 40ms default gap, so we stay ON frame 0 (the base frame is now part
    /// of the visible loop, not skipped through).
    Ubiquitty::Image::advanceFrames(&t, 260);
    img = findKittyImage(t, 7002);
    if (img) {
        CHECK(img->currentFrame == 0);
        CHECK(img->frameClockMilliseconds == 10);
    }
    /// Another 40ms (total 50 ≥ 40) advances off the base frame onto frame 1.
    Ubiquitty::Image::advanceFrames(&t, 40);
    img = findKittyImage(t, 7002);
    if (img)
        CHECK(img->currentFrame == 1);
}

TEST(testKittyGraphicsAnimationBaseFrameInLoop)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;
    int i;

    Ubiquitty::hardReset(t);
    // Base frame (frame 1), 1x1 RGBA red, no explicit gap.
    feed(t, "\033_Ga=T,t=d,f=32,s=1,v=1,i=7100;/wAA/w==\033\\");
    // 5 added frames (2..6), 120ms each.
    feed(t, "\033_Ga=f,i=7100,f=32,s=1,v=1,z=120;AP8A/w==\033\\");
    feed(t, "\033_Ga=f,i=7100,f=32,s=1,v=1,z=120;AAD//w==\033\\");
    feed(t, "\033_Ga=f,i=7100,f=32,s=1,v=1,z=120;//8A/w==\033\\");
    feed(t, "\033_Ga=f,i=7100,f=32,s=1,v=1,z=120;AP///w==\033\\");
    feed(t, "\033_Ga=f,i=7100,f=32,s=1,v=1,z=120;/////w==\033\\");

    img = findKittyImage(t, 7100);
    CHECK(img != nullptr);
    if (img) {
        // Frame list: base + 5 added = 6 frames, base first.
        CHECK(img->frameCount == 6);
        CHECK(img->frames[0].used);
        CHECK(img->frames[0].pixels == img->pixels); // frame 0 aliases base
        CHECK(!img->frames[0].gapless);
        CHECK(img->frames[0].delayMilliseconds == 0); // 0 = default gap
        // Base frame pixels are red.
        CHECK(img->frames[0].pixels[0] == 0xFF);
        CHECK(img->frames[0].pixels[1] == 0x00);
        CHECK(img->frames[0].pixels[2] == 0x00);
        for (i = 1; i < 6; i++) {
            CHECK(img->frames[i].used);
            CHECK(!img->frames[i].gapless);
            CHECK(img->frames[i].delayMilliseconds == 120);
        }
        CHECK(img->currentFrame == 0);
    }

    // a=a c=3 → looping.
    feed(t, "\033_Ga=a,i=7100,c=3;\033\\");
    img = findKittyImage(t, 7100);
    if (img) {
        CHECK(img->animationState == Ubiquitty::Image::AnimationState::Looping);
        CHECK(img->currentFrame == 0);
    }

    // Step the whole loop and record every visited frame. With the base
    // frame at the 40ms default gap and 5x 120ms frames, advancing 40ms
    // then 120ms x5 walks 0->1->2->3->4->5 and a final 120 wraps back to
    // the base (0). The base must appear in the visited sequence.
    {
        int visited[16];
        int count = 0;
        int j;
        bool sawBase = false;
        const uint32_t steps[] = { 40, 120, 120, 120, 120, 120, 120 };
        const uint16_t expectedSeq[] = { 1, 2, 3, 4, 5, 0, 1 };
        for (j = 0; j < (int)(sizeof(steps) / sizeof(steps[0])); j++) {
            Ubiquitty::Image::advanceFrames(&t, steps[j]);
            img = findKittyImage(t, 7100);
            if (img) {
                visited[count++] = img->currentFrame;
                CHECK(img->currentFrame == expectedSeq[j]);
            }
        }
        // The loop wrapped through frame 0 (the base) — it is in the cycle.
        for (j = 0; j < count; j++)
            if (visited[j] == 0)
                sawBase = true;
        CHECK(sawBase);
    }
}

TEST(testKittyGraphicsAnimationGaplessFrame)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Image::TerminalImage* img;

    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=32,s=1,v=1,i=7200;/wAA/w==\033\\");
    // Frame 1 is gapless (z=-1): base data, never dwelt on.
    feed(t, "\033_Ga=f,i=7200,f=32,s=1,v=1,z=-1;AP8A/w==\033\\");
    // Frame 2 has a real 100ms gap.
    feed(t, "\033_Ga=f,i=7200,f=32,s=1,v=1,z=100;AAD//w==\033\\");

    img = findKittyImage(t, 7200);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->frameCount == 3);
        CHECK(img->frames[1].gapless);
        CHECK(!img->frames[2].gapless);
        CHECK(img->frames[2].delayMilliseconds == 100);
    }

    feed(t, "\033_Ga=a,i=7200,c=3;\033\\");
    /// Advance just past the base frame's 40ms default gap: the 1ms surplus
    /// drives through the gapless frame 1 (instant skip, zero dwell) and
    /// lands on frame 2.
    Ubiquitty::Image::advanceFrames(&t, 41);
    img = findKittyImage(t, 7200);
    if (img)
        CHECK(img->currentFrame == 2);
}

TEST(testKittyGraphicsAnimationAndFrameErrors)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);

    // a=a (animation control) without image id: refused, and with no id to
    // answer to, unanswered.
    feed(t, "\033_Ga=a,c=2;\033\\");
    CHECK(drain(t, outputBuffer) == 0);

    // a=a with unknown image id.
    feed(t, "\033_Ga=a,i=9999,c=2;\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "ENOENT") != nullptr);

    // a=f (add frame) without image id: unanswered too.
    feed(t, "\033_Ga=f,f=24,s=1,v=1;AAAA\033\\");
    CHECK(drain(t, outputBuffer) == 0);

    // a=f with unknown image id.
    feed(t, "\033_Ga=f,i=8888,f=24,s=1,v=1;AAAA\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "ENOENT") != nullptr);

    // Create a PNG-backed image, then try to add a frame → format
    // unsupported. We don't need a valid PNG payload — the error
    // fires before the transport reads, but the image must already
    // exist with format != 24/32. Skip if PNG creation is non-trivial;
    // use direct RGBA + a separate image that fakes format=100 via
    // a malformed setup is not feasible. Instead exercise the
    // size-mismatch path: create RGB image, then ship a frame with
    // wrong pixel count.
    // 2x1 RGB = 6 bytes; base64 "AQIDBAUG" decodes to bytes 1..6.
    feed(t, "\033_Ga=T,t=d,f=24,s=2,v=1,i=42;AQIDBAUG\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 42) != nullptr);
    // Now ship a frame with payload that's the wrong size for 2x1
    // RGB. "AA==" decodes to 1 byte, expected 6 → size mismatch.
    feed(t, "\033_Ga=f,i=42,f=24,s=2,v=1;AA==\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "EINVAL:frame size mismatch") != nullptr);
}

TEST(testKittyGraphicsDeleteAll)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    // Stand up two images so "delete all" has something to do.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=11;/wAA\033\\");
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=12;AP8A\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 11) != nullptr);
    CHECK(findKittyImage(t, 12) != nullptr);
    // d=A wipes everything.
    feed(t, "\033_Ga=d,d=A;\033\\");
    CHECK(drain(t, outputBuffer) == 0); // names no image, so no reply
    CHECK(findKittyImage(t, 11) == nullptr);
    CHECK(findKittyImage(t, 12) == nullptr);

    // Bare a=d removes placements but leaves stored image data available
    // for a future a=p. Vaxis relies on this during every render.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=21;/wAA\033\\");
    drain(t, outputBuffer);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    feed(t, "\033_Ga=d\033\\");
    drain(t, outputBuffer);
    const Ubiquitty::Image::TerminalImage* img = findKittyImage(t, 21);
    CHECK(img != nullptr);
    if (img)
        CHECK(!img->displayed);
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);

    // a=d with non-existent image id → ENOENT.
    feed(t, "\033_Ga=d,d=i,i=4242;\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "ENOENT") != nullptr);
}

TEST(testKittyGraphicsDeleteByPlacement)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    // Create image + one explicit placement.
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=77,p=5;/wAA\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 77) != nullptr);
    // Delete only placement 5; image survives.
    feed(t, "\033_Ga=d,d=i,i=77,p=5;\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 77) != nullptr);
    CHECK(strstr(outputBuffer, "OK") != nullptr);
    // Lowercase d=i without a placement id removes placements only.
    feed(t, "\033_Ga=d,d=i,i=77;\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 77) != nullptr);
    // Uppercase d=I also frees the unreferenced image data.
    feed(t, "\033_Ga=d,d=I,i=77;\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 77) == nullptr);
}

TEST(testKittyGraphicsVaxisTransmitClearPut)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);

    feed(t, "\033_Gf=24,s=1,v=1,i=9001;/wAA\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 9001) != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);

    feed(t, "\033_Ga=d\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 9001) != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);

    feed(t, "\033_Ga=p,i=9001,C=1\033\\");
    drain(t, outputBuffer);
    CHECK(findKittyImage(t, 9001) != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    CHECK(strstr(outputBuffer, "i=9001;OK") != nullptr);
}

TEST(testKittyGraphicsSurviveScrollback)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=77,p=1;/wAA\033\\");
    CHECK(findKittyImage(t, 77) != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    feed(t, "L000\r\nL001\r\nL002\r\nL003\r\nL004\r\nL005");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);
    CHECK(findKittyImage(t, 77) != nullptr);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
}

TEST(testKittyGraphicsMissingSemicolon)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    feed(t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=1\033\\"); // no ';'
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "EINVAL:missing payload separator") != nullptr);
}

TEST(testKittyKeyboardPopClamp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// Pushing two distinct flag sets.
    Ubiquitty::Kitty::pushFlags(t, 1);
    Ubiquitty::Kitty::pushFlags(t, 2);
    /// Pop with count == 0 should still pop exactly one.
    Ubiquitty::Kitty::popFlags(t, 0);
    /// And pop with negative count similarly.
    Ubiquitty::Kitty::popFlags(t, -3);
    /// Stack should now be empty; a further pop is a no-op.
    Ubiquitty::Kitty::popFlags(t, 5);
}

TEST(testKittyClearedOnEd)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // 1x1 RGB placed at the cursor (a=T, f=24, s=1, v=1, payload = one
    // white pixel "/wAA" as used by the placement tests).
    feed(t, "\033[H\033_Ga=T,f=24,s=1,v=1;/wAA\033\\");
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    feed(t, "\033[2J");
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
}

TEST(testXTGETTCAPDoesNotBreakSixel)
{
    /// Sanity: plain `DCS q ... ST` (no `+` intermediate) still routes
    /// to the sixel decoder, not XTGETTCAP. After the frame, no DCS
    /// response should appear in outputBuffer because sixel produces no
    /// host response.
    Ubiquitty::Terminal t;
    char outputBuffer[16];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033Pq~\033\\");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testSixelClipDiagnostic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureDiagClient diag;
    t.diagnosticsClient = &diag;
    std::string frame = "\033Pq";
    frame.append(2100, '~');
    frame.append("\033\\");
    Ubiquitty::processBytes(&t, frame);
    CHECK(diag.sixelClipped == 0);
    const auto* image = Ubiquitty::Image::stored(&t, 0);
    CHECK(image);
    CHECK(image->width == 2100);
    // The default sixel aspect doubles the six decoded rows vertically.
    CHECK(image->height == 12);
    feed(t, "\033Pq~~~\033\\");
    CHECK(diag.sixelClipped == 0);
    t.diagnosticsClient = nullptr;
}

TEST(testTekBrightPenOverride)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.tekBrightPenOverride == 0);
    feed(t, "\033[?7400h"); // force on
    CHECK(t.tekBrightPenOverride == 1);
    feed(t, "\033[?7400l"); // force off
    CHECK(t.tekBrightPenOverride == 2);
    feed(t, "\033[!p"); // DECSTR soft reset → back to config default
    CHECK(t.tekBrightPenOverride == 0);
    // Tek-scoped: set in VT mode (CSI isn't parsed once in Tek), persists into
    // Tek, and clears automatically on Tek exit (ESC ETX).
    feed(t, "\033[?7400l");
    feed(t, "\033[?38h"); // enter Tek — the override carries in
    CHECK(t.tekBrightPenOverride == 2);
    feed(t, "\033\003"); // ESC ETX — exit Tek → override cleared to config
    CHECK(t.tekBrightPenOverride == 0);
}

// ---- a=f budget + per-image ceiling -----------------------------------------

// zlib-then-base64, the wire form an o=z payload takes. Written here rather than
// pasted as a literal because these payloads describe multi-megabyte images: the
// point of o=z is that the compressed form is small, so building it is cheaper
// than carrying it.
static std::string zlibBase64(const std::vector<std::uint8_t>& raw)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::vector<std::uint8_t> deflated = Ubiquitty::Codec::Zlib::compress({ raw.data(), raw.size() });
    std::string out;
    out.reserve((deflated.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < deflated.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(deflated[i]) << 16) | (static_cast<std::uint32_t>(deflated[i + 1]) << 8) | deflated[i + 2];
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(alphabet[(triple >> 6) & 0x3F]);
        out.push_back(alphabet[triple & 0x3F]);
    }
    if (i + 1 == deflated.size()) {
        const std::uint32_t triple = static_cast<std::uint32_t>(deflated[i]) << 16;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.append("==");
    } else if (i + 2 == deflated.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(deflated[i]) << 16) | (static_cast<std::uint32_t>(deflated[i + 1]) << 8);
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(alphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// An animation's frames are the same image, so maxDecodedImageBytes has to bound
// the image including them: each frame is exactly width*height*bytesPerPixel, and
// maxTerminalImageFrameCount of them reach 16x the ceiling. o=z is what makes that
// cheap for a hostile program — the wire cost is a few KB per multi-megabyte frame,
// which is why this case builds its payload the same way.
TEST(testKittyFrameRefusedPastTheMaxPerImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // 2048x2048 RGBA is 16 MiB, so base + 3 frames sits exactly on the 64 MiB
    // ceiling and a fourth frame is 16 MiB past it.
    const int side = 2048;
    const std::size_t frameBytes = static_cast<std::size_t>(side) * side * 4u;
    std::vector<std::uint8_t> raw(frameBytes);
    for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<std::uint8_t>(i & 0xFF); // compressible, not degenerate
    const std::string payload = zlibBase64(raw);

    std::string command = "\033_Ga=T,t=d,f=32,o=z,s=2048,v=2048,i=9100;" + payload + "\033\\";
    feed(t, command.c_str());
    const Ubiquitty::Image::TerminalImage* img = findKittyImage(t, 9100);
    CHECK(img != nullptr);
    if (!img)
        return;
    CHECK(Ubiquitty::Image::imageBytes(*img) == frameBytes);

    const std::string frame = "\033_Ga=f,i=9100,f=32,o=z,s=2048,v=2048,z=40;" + payload + "\033\\";
    for (int accepted = 1; accepted <= 3; ++accepted) {
        feed(t, frame.c_str());
        img = findKittyImage(t, 9100);
        CHECK(img != nullptr);
        if (img)
            CHECK(img->frameCount == accepted + 1);
    }
    img = findKittyImage(t, 9100);
    CHECK(img != nullptr);
    if (img)
        CHECK(Ubiquitty::Image::imageBytes(*img) == frameBytes * 4); // exactly the ceiling

    // One more would pass it, so it is refused and the image is left as it was.
    feed(t, frame.c_str());
    img = findKittyImage(t, 9100);
    CHECK(img != nullptr);
    if (img) {
        CHECK(img->frameCount == 4);
        CHECK(Ubiquitty::Image::imageBytes(*img) == frameBytes * 4);
    }
}

// A frame adds to the Terminal total, so the budget needs enforcing on this path
// as it is after every other install. Tiny images keep the case cheap: the budget
// is a field, so it can be set below what two of them cost.
TEST(testKittyFrameEnforcesTheTerminalBudget)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Each 1x1 f=24 image is 3 bytes. Two fit in 8; adding a frame to one makes 9.
    t.images.kittyImageBytesBudget = 8;

    feed(t, "\033_Ga=t,t=d,f=24,s=1,v=1,i=9200;/wAA\033\\"); // transmit only
    feed(t, "\033_Ga=t,t=d,f=24,s=1,v=1,i=9201;AP8A\033\\");
    CHECK(findKittyImage(t, 9200) != nullptr);
    CHECK(findKittyImage(t, 9201) != nullptr);

    // The frame pushes the total to 9 > 8, so the older image is evicted rather
    // than the budget being quietly exceeded.
    feed(t, "\033_Ga=f,i=9201,f=24,s=1,v=1,z=40;AAD/\033\\");
    CHECK(findKittyImage(t, 9201) != nullptr); // the one being extended survives
    CHECK(findKittyImage(t, 9200) == nullptr); // the older one paid for it
    CHECK(Ubiquitty::Image::totalBytes(&t) <= t.images.kittyImageBytesBudget);
}

TEST(testKittyAPCPayloadPastCapacityIsDropped)
{
    // A payload past maxAPCCapacity: the batched append fails and sets the
    // overflow latch; every byte after it is consumed and dropped, and the
    // terminator returns without dispatching.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033_Gf=24;"); // open an APC
    constexpr int chunkSize = 1024 * 1024;
    const int chunks = Ubiquitty::Kitty::maxAPCCapacity / chunkSize + 2;
    std::string chunk(static_cast<std::size_t>(chunkSize), 'a');
    for (int i = 0; i < chunks; i++)
        feed(t, chunk);
    CHECK(t.images.kittyAPCOverflow == true);
    feed(t, "x"); // post-overflow byte — dropped per byte
    CHECK(t.images.kittyAPC.length == 0);
    feed(t, "\033\\");
    CHECK(t.images.kittyAPCOverflow == true); // still latched, nothing dispatched
}

TEST(testKittyAPCStrayEscapeEndsTheStringShortOfCapacity)
{
    // An ESC that is not the terminator's ends the string where it stands:
    // the payload dispatches as it is, the buffer never holds the ESC or the
    // byte after it, and that byte begins its own sequence — here SOS, which
    // the terminator that follows ends. Nothing latches, so the next APC
    // starts from an empty buffer.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033_G;"); // payload "G;" — 'G' is the first payload byte
    constexpr int chunkSize = 1024 * 1024;
    const int fullChunks = Ubiquitty::Kitty::maxAPCCapacity / chunkSize - 1; // 63 chunks = 63 MiB
    std::string chunk(static_cast<std::size_t>(chunkSize), 'a');
    for (int i = 0; i < fullChunks; i++)
        feed(t, chunk);
    const int remainderBytes = Ubiquitty::Kitty::maxAPCCapacity - 1 - fullChunks * chunkSize - 2; // 1 MiB - 3 (payload "G;" included)
    std::string remainder(static_cast<std::size_t>(remainderBytes), 'a');
    feed(t, remainder); // capacity - 1
    CHECK_EQ(t.images.kittyAPC.length, Ubiquitty::Kitty::maxAPCCapacity - 1);
    feed(t, "\033X"); // the string ends at the ESC; 'X' opens SOS
    CHECK(t.images.kittyAPCOverflow == false);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::SOS);
    feed(t, "y\033\\"); // SOS body, then its terminator
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    feed(t, "\033_Ga=q;\033\\");
    CHECK(t.images.kittyAPCOverflow == false);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

UBIQUITTY_TEST_MAIN()
