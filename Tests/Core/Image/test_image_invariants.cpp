// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The invariants an image keeps between the escape that transmits it and the
// quad that draws it, held across every protocol that can produce one.
//
// One round of manual testing produced five separate placement defects — an
// iTerm2 image that showed and vanished, kitty images stretched along one axis,
// duplicate placements after scrolling, a kitty image drawn under a sixel, and
// wrong colors — because nothing pinned the middle of the pipeline. The model in
// that middle is protocol-agnostic by design (TerminalImage + placements +
// resolveImagePlacements), so the invariants are too: what a placement means,
// where it lands, how it survives scrolling, and when it is allowed to die.
//
// A case here states one such invariant and holds every ingress to it.

#include "Harness.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Ubiquitty::Image::ImageQuad;
using Ubiquitty::Image::maxTerminalImagePlacementCount;
using Ubiquitty::Image::TerminalImagePlacement;

// The store frees pixels with free(), so a test buffer has to come from malloc.
unsigned char* pixelBuffer(std::size_t bytes)
{
    auto* p = static_cast<unsigned char*>(std::malloc(bytes ? bytes : 1));
    CHECK(p != nullptr);
    std::memset(p, 0xFF, bytes ? bytes : 1);
    return p;
}

void feed(Ubiquitty::Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, std::string_view(bytes));
}

int livePlacements(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            ++n;
    return n;
}

// The one geometry every case resolves against: 10x10 device-pixel cells, a
// grid origin at the drawable's corner, and one device pixel per point, so a
// quad's numbers are readable as cells without a scale factor in the way.
struct Geometry {
    Ubiquitty::GridSize grid { .columns = 80, .rows = 24 };
    Ubiquitty::FloatSize cell { 10.0f, 10.0f };
    Ubiquitty::FloatPoint origin { 0.0f, 0.0f };
    int viewOffset = 0;
};

int resolve(const Ubiquitty::Terminal& t, const Geometry& geometry, std::span<ImageQuad> out)
{
    return Ubiquitty::Image::resolveImagePlacements(t.images.stored, t.images.placements,
        geometry.grid, geometry.cell, geometry.origin, geometry.viewOffset, out);
}

} // namespace

// An image the terminal has been told to draw at a cell is drawn at that cell.
// The anchor is where the cursor was, and a quad landing anywhere else is an
// image the user sees in the wrong place.
TEST(testAPlacementResolvesAtItsAnchorCell)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.screens.active->cursorX = 3;
    t.screens.active->cursorY = 5;

    // 40x20 pixels into a 4x2 cell box, at 10x10 cells.
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));

    Geometry geometry;
    std::array<ImageQuad, 8> quads {};
    CHECK(resolve(t, geometry, quads) == 1);
    CHECK(quads[0].minXInPixels == 30.0f); // column 3 * 10
    CHECK(quads[0].minYInPixels == 50.0f); // row 5 * 10
    CHECK(quads[0].maxXInPixels == 70.0f); // 4 cells wide
    CHECK(quads[0].maxYInPixels == 70.0f); // 2 cells tall
}

// An image that arrived without an id (a sixel, an OSC 1337 file) is not the
// image a kitty client means when it names id 1. Client ids are every id below
// 2^31 and clients number from one, so the terminal's own ids start above: an
// inline image that took id 1 was found and replaced by the client's first
// transmit, and the client's box then showed the inline image's pixels.
TEST(testAnUnnamedImageIsNotTheOneAClientNamesFirst)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));
    std::uint32_t inlineID = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used)
            inlineID = t.images.stored[i].imageID;
    CHECK(inlineID >= 0x80000000u);

    // A 1x1 RGBA kitty transmit under the first id a client would pick.
    feed(t, "\033_Ga=T,f=32,s=1,v=1,i=1,q=2;AAAAAA==\033\\");
    int used = 0;
    const Ubiquitty::Image::TerminalImage* inlineImage = nullptr;
    const Ubiquitty::Image::TerminalImage* clientImage = nullptr;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        const Ubiquitty::Image::TerminalImage& image = t.images.stored[i];
        if (!image.used)
            continue;
        ++used;
        if (image.imageID == inlineID)
            inlineImage = &image;
        if (image.imageID == 1)
            clientImage = &image;
    }
    CHECK(used == 2);
    CHECK(inlineImage != nullptr && inlineImage->width == 40 && inlineImage->height == 20);
    CHECK(clientImage != nullptr && clientImage->width == 1 && clientImage->height == 1);
    CHECK(livePlacements(t) == 2);
}

// A cell box states the image's size on both axes at once, or on neither. Sizing width
// from cells while height comes from source pixels (or the reverse) is a
// stretch along one axis — the shape of the kitty stretch report — and it is
// invisible at scale 1 with square cells, which is why the case sets neither.
TEST(testACellBoxSizesBothAxesOnTheSameBasis)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // A 40x20 image is 4x2 cells at these metrics, so a correct resolve gives
    // the same rectangle whichever way the box is expressed.
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));

    Geometry geometry;
    geometry.cell = { 10.0f, 10.0f };
    std::array<ImageQuad, 8> quads {};
    CHECK(resolve(t, geometry, quads) == 1);
    const float boxedWidth = quads[0].maxXInPixels - quads[0].minXInPixels;
    const float boxedHeight = quads[0].maxYInPixels - quads[0].minYInPixels;

    // The same image with no cell box at all: natural pixels, same aspect.
    Ubiquitty::Terminal natural;
    Ubiquitty::hardReset(natural);
    CHECK(Ubiquitty::Image::storeRGBAImage(&natural, pixelBuffer(40 * 20 * 4), 40, 20, 0, 0));
    std::array<ImageQuad, 8> naturalQuads {};
    CHECK(resolve(natural, geometry, naturalQuads) == 1);
    const float naturalWidth = naturalQuads[0].maxXInPixels - naturalQuads[0].minXInPixels;
    const float naturalHeight = naturalQuads[0].maxYInPixels - naturalQuads[0].minYInPixels;

    // Both routes describe the same picture, so both must have the same aspect.
    CHECK(boxedWidth * naturalHeight == naturalWidth * boxedHeight);
}

// Half a cell box is the stretch itself: width in cells, height in pixels.
// resolveImagePlacements takes the two independently, so a protocol handler
// that fills one and leaves the other zero produces a distorted image with no
// other symptom. The invariant is that a placement never reaches the resolver
// half-boxed.
TEST(testNoPlacementReachesTheResolverWithHalfACellBox)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));

    for (int i = 0; i < maxTerminalImagePlacementCount; ++i) {
        const TerminalImagePlacement& p = t.images.placements[i];
        if (!p.used)
            continue;
        CHECK((p.cellColumns > 0) == (p.cellRows > 0));
    }
}

// Scrolling moves a placement; it never copies one. Every scroll shifts the
// anchor by exactly the rows scrolled, and the number of placements is what it
// was — the duplication report is this invariant failing.
TEST(testScrollingMovesPlacementsWithoutDuplicatingThem)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.screens.active->cursorX = 0;
    t.screens.active->cursorY = 10;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));
    CHECK(livePlacements(t) == 1);

    const int startRow = t.images.placements[0].cursorY;
    for (int scrolled = 1; scrolled <= 5; ++scrolled) {
        Ubiquitty::Image::scrollPlacementsUp(t, 1);
        CHECK(livePlacements(t) == 1);
        int found = 0;
        for (int i = 0; i < maxTerminalImagePlacementCount; ++i) {
            if (!t.images.placements[i].used)
                continue;
            ++found;
            CHECK(t.images.placements[i].cursorY == startRow - scrolled);
        }
        CHECK(found == 1);
    }

    // And one quad throughout: a duplicate would show as a second draw even
    // when the placement table looks right.
    Geometry geometry;
    std::array<ImageQuad, 8> quads {};
    CHECK(resolve(t, geometry, quads) == 1);
}

// A placement scrolled out of the viewport but still inside retained scrollback
// stays alive, and comes back at the right row when the view scrolls back to
// it. The viewOffset is what makes an image in history visible again, so the
// two have to agree: offset N cancels N rows of scroll exactly.
TEST(testAScrolledPlacementReturnsAtItsOriginalRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.screens.active->cursorX = 2;
    t.screens.active->cursorY = 4;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));

    Geometry geometry;
    std::array<ImageQuad, 8> before {};
    CHECK(resolve(t, geometry, before) == 1);

    Ubiquitty::Image::scrollPlacementsUp(t, 6); // now above the viewport
    CHECK(livePlacements(t) == 1);

    geometry.viewOffset = 6; // scrolled back to where it was
    std::array<ImageQuad, 8> after {};
    CHECK(resolve(t, geometry, after) == 1);
    CHECK(after[0].minXInPixels == before[0].minXInPixels);
    CHECK(after[0].minYInPixels == before[0].minYInPixels);
    CHECK(after[0].maxXInPixels == before[0].maxXInPixels);
    CHECK(after[0].maxYInPixels == before[0].maxYInPixels);
}

// Slot pressure preserves every image intersecting the live screen.
TEST(testAVisibleImageIsNotEvictedForANewOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Fill every slot with a displayed image, each on its own row.
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        t.screens.active->cursorX = 0;
        t.screens.active->cursorY = i % t.rows;
        CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(10 * 10 * 4), 10, 10, 1, 1));
    }
    const int displayedBefore = livePlacements(t);
    CHECK(displayedBefore == Ubiquitty::Image::maxTerminalImageCount);

    // One more transmit. It may be refused — that is the store's documented
    // answer when nothing is evictable — but it must not take a visible image
    // with it.
    unsigned char* extra = pixelBuffer(10 * 10 * 4);
    t.screens.active->cursorY = 20;
    const bool didStoreExtra = Ubiquitty::Image::storeRGBAImage(&t, extra, 10, 10, 1, 1);
    if (!didStoreExtra)
        std::free(extra);
    CHECK(livePlacements(t) >= displayedBefore);
}

TEST(testNineImagesRemainStored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int index = 0; index < 9; ++index) {
        t.screens.active->cursorY = index;
        auto* pixels = pixelBuffer(4);
        const bool didStore = Ubiquitty::Image::storeRGBAImage(&t, pixels, 1, 1, 1, 1);
        if (!didStore) {
            std::free(pixels);
        }
        CHECK(didStore);
    }
    CHECK(livePlacements(t) == 9);
}

TEST(testSlotPressureEvictsHistoryBeforeTheLiveScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(4), 1, 1, 1, 1));
    const auto historyID = t.images.placements[0].imageID;
    Ubiquitty::Image::scrollPlacementsUp(t, 2);
    CHECK(livePlacements(t) == 1);
    for (int index = 1; index < Ubiquitty::Image::maxTerminalImageCount; ++index) {
        t.screens.active->cursorY = index % t.rows;
        CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(4), 1, 1, 1, 1));
    }
    auto* pixels = pixelBuffer(4);
    const bool didStore = Ubiquitty::Image::storeRGBAImage(&t, pixels, 1, 1, 1, 1);
    if (!didStore) {
        std::free(pixels);
    }
    CHECK(didStore);
    CHECK(livePlacements(t) == Ubiquitty::Image::maxTerminalImageCount);
    for (const auto& placement : t.images.placements) {
        CHECK(!placement.used || placement.imageID != historyID);
    }
}

// The byte budget evicts, but never a picture on screen while an off-screen one
// is available. Pass 0 of the eviction loop exists for exactly this, and the
// case pins it: the image nothing is drawing goes first.
TEST(testTheBudgetEvictsAnOffscreenImageBeforeAVisibleOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.images.kittyImageBytesBudget = 9u * 1000u * 1000u; // two 4 MB images fit, three do not

    // One visible image.
    t.screens.active->cursorY = 0;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 1000, 1000, 1, 1));
    const uint32_t visibleID = t.images.placements[0].imageID;

    // One image with no placement: scrolled far past retention, so the store
    // holds pixels nothing draws.
    t.screens.active->cursorY = 1;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 1000, 1000, 1, 1));
    Ubiquitty::Image::clearPlacementsInRows(t, 1, 1);

    // A third forces an eviction. The undisplayed one must be the victim.
    t.screens.active->cursorY = 2;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(16), 1000, 1000, 1, 1));

    bool visibleSurvives = false;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used && t.images.stored[i].imageID == visibleID)
            visibleSurvives = true;
    CHECK(visibleSurvives);
}

// Erasing the rows an image sits on removes it, and erasing rows elsewhere
// leaves it alone. A clear that reaches too far is the other way an image
// vanishes while its anchor is still on screen.
TEST(testAnEraseRemovesOnlyTheImagesItCovers)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    t.screens.active->cursorX = 0;
    t.screens.active->cursorY = 2;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2)); // rows 2-3
    t.screens.active->cursorY = 10;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2)); // rows 10-11
    CHECK(livePlacements(t) == 2);

    Ubiquitty::Image::clearPlacementsInRows(t, 10, 11);
    CHECK(livePlacements(t) == 1);
    for (int i = 0; i < maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            CHECK(t.images.placements[i].cursorY == 2);

    Ubiquitty::Image::clearPlacementsInRows(t, 20, 22); // nowhere near it
    CHECK(livePlacements(t) == 1);
}

// A placement's quad tracks the cell metrics: the same placement under bigger
// cells covers proportionally more pixels on both axes, and nothing else moves.
// A font resize is the everyday version of this, and an axis that fails to
// track is the stretch again, one resize later.
TEST(testQuadGeometryTracksCellMetricsOnBothAxes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.screens.active->cursorX = 1;
    t.screens.active->cursorY = 1;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));

    Geometry small;
    small.cell = { 10.0f, 20.0f };
    std::array<ImageQuad, 8> smallQuads {};
    CHECK(resolve(t, small, smallQuads) == 1);

    Geometry large;
    large.cell = { 20.0f, 40.0f }; // exactly twice, both axes
    std::array<ImageQuad, 8> largeQuads {};
    CHECK(resolve(t, large, largeQuads) == 1);

    CHECK((largeQuads[0].maxXInPixels - largeQuads[0].minXInPixels)
        == 2.0f * (smallQuads[0].maxXInPixels - smallQuads[0].minXInPixels));
    CHECK((largeQuads[0].maxYInPixels - largeQuads[0].minYInPixels)
        == 2.0f * (smallQuads[0].maxYInPixels - smallQuads[0].minYInPixels));
    CHECK(largeQuads[0].minXInPixels == 2.0f * smallQuads[0].minXInPixels);
    CHECK(largeQuads[0].minYInPixels == 2.0f * smallQuads[0].minYInPixels);
}

// Every resolved quad names an image the store still holds. A quad for a
// disposed image draws whatever now occupies that texture slot, which is the
// mechanism behind an image appearing where a different one was.
TEST(testEveryQuadNamesALiveImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int i = 0; i < 4; ++i) {
        t.screens.active->cursorX = 0;
        t.screens.active->cursorY = i * 3;
        CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2));
    }
    Ubiquitty::Image::scrollPlacementsUp(t, 4);

    Geometry geometry;
    std::array<ImageQuad, 16> quads {};
    const int count = resolve(t, geometry, quads);
    for (int q = 0; q < count; ++q) {
        bool isNamed = false;
        for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
            const Ubiquitty::Image::TerminalImage& image = t.images.stored[i];
            if (image.used && image.pixels && image.imageID == quads[q].imageID)
                isNamed = true;
        }
        CHECK(isNamed);
    }
}

// --- ingest: the invariants hold for the bytes a real emitter sends ----------

// However a transmit states its box — both axes, one, or neither — the drawn
// rectangle keeps the image's aspect. A protocol may name only the width and
// leave the height to the terminal, and the two axes are sized independently
// downstream, so the axis nobody stated is exactly where a stretch comes from.
TEST(testATransmitKeepsItsAspectHoweverItsBoxIsStated)
{
    // A 4:1 image, so a mixed basis is unmistakable in the ratio.
    const char* transmits[] = {
        "\033_Ga=T,i=1,f=24,s=4,v=1,c=4,r=1;AAAAAAAAAAAAAAAA\033\\", // both axes
        "\033_Ga=T,i=2,f=24,s=4,v=1;AAAAAAAAAAAAAAAA\033\\", // neither
        "\033_Ga=T,i=3,f=24,s=4,v=1,c=8;AAAAAAAAAAAAAAAA\033\\", // width only
        "\033_Ga=T,i=4,f=24,s=4,v=1,r=2;AAAAAAAAAAAAAAAA\033\\", // height only
    };
    for (const char* transmit : transmits) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, transmit);
        CHECK(livePlacements(t) == 1);

        // Square cells, so cell counts and pixels agree and the ratio below is
        // the image's own.
        Geometry geometry;
        geometry.cell = { 10.0f, 10.0f };
        std::array<ImageQuad, 8> quads {};
        CHECK(resolve(t, geometry, quads) == 1);
        const float width = quads[0].maxXInPixels - quads[0].minXInPixels;
        const float height = quads[0].maxYInPixels - quads[0].minYInPixels;
        CHECK(width > 0.0f && height > 0.0f);
        // 4:1, within the rounding a cell-quantized box can introduce.
        const float ratio = width / height;
        CHECK(ratio > 3.5f && ratio < 4.5f);
    }
}

// Images flow with the text under every scroll a shell can cause. A newline at
// the bottom of the screen scrolls; so does a newline at the bottom of a
// DECSTBM region. A placement that fails to move stays pinned to a grid row
// while the text slides out from under it, and the next image lands beside it —
// which reads as the picture duplicating.
TEST(testAPlacementFollowsTheTextUnderEveryScroll)
{
    struct Case {
        const char* label;
        const char* setup;
    };
    const Case cases[] = {
        { "full screen", "" },
        { "DECSTBM region", "\033[1;24r" },
    };
    for (const Case& oneCase : cases) {
        (void)oneCase.label;
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        if (*oneCase.setup)
            feed(t, oneCase.setup);

        // Put an image near the top, then scroll the screen by ten lines.
        feed(t, "\033[3;1H");
        feed(t, "\033_Ga=T,i=9,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        int anchor = -1;
        for (int i = 0; i < maxTerminalImagePlacementCount; ++i)
            if (t.images.placements[i].used)
                anchor = t.images.placements[i].cursorY;
        CHECK(anchor >= 0);

        feed(t, "\033[24;1H"); // last row
        for (int line = 0; line < 10; ++line)
            feed(t, "x\r\n");

        // Ten scrolls, so the anchor is ten rows higher. A pinned placement
        // still reads its original row.
        for (int i = 0; i < maxTerminalImagePlacementCount; ++i)
            if (t.images.placements[i].used)
                CHECK(t.images.placements[i].cursorY == anchor - 10);
    }
}

// Re-transmitting under an id already on screen replaces that picture; it does
// not add a second placement beside it. An emitter redrawing in place (the
// animation pattern, and what a repeated demo does) must not accumulate.
TEST(testARetransmitUnderTheSameIDDoesNotAccumulatePlacements)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;1H");
    feed(t, "\033_Ga=T,i=77,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
    const int afterFirst = livePlacements(t);
    CHECK(afterFirst == 1);

    for (int repeat = 0; repeat < 5; ++repeat) {
        feed(t, "\033[5;1H");
        feed(t, "\033_Ga=T,i=77,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
    }
    // One picture at one place, however many times it was sent there.
    CHECK(livePlacements(t) == afterFirst);
}

TEST(testSixelNaturalSizeDoesNotRoundItsAspectToCells)
{
    for (const auto& dimensions : { std::pair { 120, 60 }, std::pair { 90, 90 }, std::pair { 60, 120 } }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        t.cellPixelWidth = 9;
        t.cellPixelHeight = 20;
        const auto [width, height] = dimensions;
        const std::string band(static_cast<std::size_t>(width), '~');
        std::string bytes = "\033P0;1;0q\"1;1;" + std::to_string(width) + ";" + std::to_string(height) + "#0;2;100;0;0#0";
        for (int row = 0; row < height / 6; ++row) {
            if (row) {
                bytes += '-';
            }
            bytes += band;
        }
        bytes += "\033\\";
        feed(t, bytes.c_str());
        Geometry geometry;
        geometry.cell = { 9, 20 };
        std::array<ImageQuad, 1> quads {};
        CHECK(resolve(t, geometry, quads) == 1);
        CHECK(quads[0].maxXInPixels - quads[0].minXInPixels == width);
        CHECK(quads[0].maxYInPixels - quads[0].minYInPixels == height);
    }
}

TEST(testSixelPixelAspectIsNormalizedAndBounded)
{
    struct AspectCase {
        int numerator;
        int denominator;
        int height;
    };
    for (const auto& aspect : { AspectCase { 2, 1, 120 }, AspectCase { 1, 2, 30 }, AspectCase { 3, 2, 90 }, AspectCase { 65535, 1, 0 } }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        const std::string bytes = "\033P0;1;0q\"" + std::to_string(aspect.numerator) + ";" + std::to_string(aspect.denominator)
            + ";120;60#0;2;100;0;0#0!120~\033\\";
        feed(t, bytes.c_str());
        CHECK(Ubiquitty::Image::placementCount(&t) == (aspect.height ? 1 : 0));
        if (aspect.height) {
            const auto* image = Ubiquitty::Image::stored(&t, 0);
            CHECK(image && image->width == 120 && image->height == aspect.height);
        }
    }
}

TEST(testSlotPressureProtectsAPartiallyVisibleNaturalImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.cellPixelHeight = 20;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(240), 1, 60, 0, 0));
    const auto visibleID = t.images.placements[0].imageID;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(4), 1, 1, 0, 0));
    const auto historyID = t.images.placements[1].imageID;
    Ubiquitty::Image::scrollPlacementsUp(t, 2);
    for (int index = 2; index < Ubiquitty::Image::maxTerminalImageCount; ++index) {
        CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(4), 1, 1, 1, 1));
    }
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(4), 1, 1, 1, 1));
    bool hasVisibleImage = false;
    for (const auto& image : t.images.stored) {
        CHECK(!image.used || image.imageID != historyID);
        hasVisibleImage |= image.used && image.imageID == visibleID;
    }
    CHECK(hasVisibleImage);
}

UBIQUITTY_TEST_MAIN()

// The erase that reaches placements is the whole-screen one. ED 0 and ED 1
// leave every image alone whatever rows they cover, which is kitty's rule and
// what a shell's per-prompt erase below relies on.
TEST(testOnlyAWholeScreenEraseRemovesImages)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    t.screens.active->cursorX = 0;
    t.screens.active->cursorY = 2;
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixelBuffer(40 * 20 * 4), 40, 20, 4, 2)); // rows 2-3
    feed(t, "\033[1;1H\033[J"); // erase below, from above the image
    CHECK_MSG(livePlacements(t) == 1, "an erase below across the image keeps it");
    feed(t, "\033[8;1H\033[1J"); // erase above, from below the image
    CHECK_MSG(livePlacements(t) == 1, "an erase above across the image keeps it");
    feed(t, "\033[2J");
    CHECK_MSG(livePlacements(t) == 0, "a whole-screen erase removes it");
}
