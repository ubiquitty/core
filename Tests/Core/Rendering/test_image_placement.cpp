// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Operations.h"
#include "Harness.h"
#include "TerminalFixture.h"
#include <memory>

using UbiquittyTest::feed;

// test_image_placement.cpp — the portable image-placement resolver
// (Source/Core/Image/ImagePlacement) through the frame entry point that drives
// it (Source/Core/Rendering/ImageQuads). Builds a Terminal, places an RGBA image
// through the shared store (the same path kitty/iTerm/sixel feed), and checks
// collectImageQuads() produces the right device-pixel quad: cell-span sizing,
// anchoring, viewOffset scroll, and viewport clipping. Frontend-agnostic — no
// Metal/Vulkan — so the macOS and Linux renderers share it.
#include "Core/CellGrid.h"
#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Rendering/ImageUploadStaging.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Encodings.h"
#include "Core/VT/Constants.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::unique_ptr<Ubiquitty::Terminal> makeTerm(int rows, int columns)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

// Place a 40x20px RGBA image spanning 4x2 cells at the cursor (home).
static void placeImage(Ubiquitty::Terminal& t, int cellColumns, int cellRows)
{
    unsigned char* px = static_cast<unsigned char*>(std::malloc(40u * 20u * 4u));
    std::memset(px, 0xFF, 40u * 20u * 4u);
    bool ok = Ubiquitty::Image::storeRGBAImage(&t, px, 40, 20, cellColumns, cellRows);
    CHECK(ok);
}

constexpr std::string_view naturalImagePNG = "iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAIAAABvFaqvAAAAIElEQVR4nGP4z8BAFUQdU0YNGjVo1KBRg0YNGjWIIgQABMo93z5bbNQAAAAASUVORK5CYII=";

TEST(naturalImagesUseDevicePixelsAtEveryDisplayScale)
{
    const std::array<std::string, 3> sequences {
        "\033_Ga=T,f=100;" + std::string(naturalImagePNG) + "\033\\",
        "\033]1337;File=inline=1:" + std::string(naturalImagePNG) + "\007",
        "\033P0;1q\"1;1;24;24#1;2;100;0;0!24~-!24~-!24~-!24~\033\\",
    };
    for (const float scale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        const Ubiquitty::FloatSize cell { 8.0f * scale, 16.0f * scale };
        for (const auto& sequence : sequences) {
            auto terminal = makeTerm(24, 80);
            terminal->cellPixelWidth = static_cast<int>(cell.width);
            terminal->cellPixelHeight = static_cast<int>(cell.height);
            feed(*terminal, "\033[2;2H");
            feed(*terminal, sequence);
            Ubiquitty::drainAsyncDecodes(*terminal);

            const int occupiedRows = static_cast<int>(std::ceil(24.0f / cell.height));
            CHECK_EQ(terminal->screens.active->cursorY, occupiedRows);
            feed(*terminal, "\r\nAFTER");
            CHECK_EQ(terminal->screens.active->cursorY, occupiedRows + 1);
            std::array<Ubiquitty::Image::ImageQuad, 1> quads {};
            REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), cell, {}, scale, 0, quads) == 1);
            CHECK_EQ(quads[0].minXInPixels, cell.width);
            CHECK_EQ(quads[0].minYInPixels, cell.height);
            CHECK_EQ(quads[0].maxXInPixels - quads[0].minXInPixels, 24.0f);
            CHECK_EQ(quads[0].maxYInPixels - quads[0].minYInPixels, 24.0f);
            CHECK(quads[0].maxYInPixels <= terminal->screens.active->cursorY * cell.height);
        }
    }
}

TEST(kittyPixelOffsetsAndSourceRectanglesUseDevicePixels)
{
    for (const float scale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        auto terminal = makeTerm(24, 80);
        terminal->cellPixelWidth = static_cast<int>(12.0f * scale);
        terminal->cellPixelHeight = static_cast<int>(32.0f * scale);
        feed(*terminal, "\033_Ga=T,i=7,f=100,C=1;" + std::string(naturalImagePNG) + "\033\\");
        Ubiquitty::drainAsyncDecodes(*terminal);
        feed(*terminal, "\033[2;3H");
        feed(*terminal, "\033_Ga=p,i=7,p=2,C=1,x=2,y=3,w=12,h=7,X=3,Y=4\033\\");
        std::array<Ubiquitty::Image::ImageQuad, 2> quads {};
        const Ubiquitty::FloatSize cell { 12.0f * scale, 32.0f * scale };
        REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), cell, { 5.0f, 9.0f }, scale, 0, quads) == 2);
        CHECK_EQ(quads[1].minXInPixels, 5.0f + 2.0f * cell.width + 3.0f);
        CHECK_EQ(quads[1].minYInPixels, 9.0f + cell.height + 4.0f);
        CHECK_EQ(quads[1].maxXInPixels - quads[1].minXInPixels, 12.0f);
        CHECK_EQ(quads[1].maxYInPixels - quads[1].minYInPixels, 7.0f);
        CHECK(std::fabs(quads[1].u0 - 2.0f / 24.0f) < 0.0001f);
        CHECK(std::fabs(quads[1].v0 - 3.0f / 24.0f) < 0.0001f);
        CHECK(std::fabs(quads[1].u1 - 14.0f / 24.0f) < 0.0001f);
        CHECK(std::fabs(quads[1].v1 - 10.0f / 24.0f) < 0.0001f);
    }
}

TEST(kittySingleAxisPlacementsPreserveAspectWithFractionalCells)
{
    for (const char* dimensions : { "c=3", "r=2" }) {
        auto terminal = makeTerm(24, 80);
        terminal->cellPixelWidth = 15;
        terminal->cellPixelHeight = 34;
        feed(*terminal, std::string("\033_Ga=T,f=24,s=1,v=1,C=1,") + dimensions + ";/wAA\033\\");
        Ubiquitty::drainAsyncDecodes(*terminal);
        Ubiquitty::Image::ImageQuad quads[1];
        REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), { 14.76f, 34.0f }, {}, 1.0f, 0, quads) == 1);
        const float width = quads[0].maxXInPixels - quads[0].minXInPixels;
        const float height = quads[0].maxYInPixels - quads[0].minYInPixels;
        CHECK(width == height);
        CHECK(width == (dimensions[0] == 'c' ? 3 * 14.76f : 2 * 34.0f));
    }
}

TEST(uniformFitScalesIntrinsicImagePixelsOffsetsAndCropsTogether)
{
    for (const float backingScale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        auto terminal = makeTerm(24, 80);
        const Ubiquitty::FloatSize naturalCell { 12 * backingScale, 32 * backingScale };
        terminal->cellPixelWidth = static_cast<int>(naturalCell.width);
        terminal->cellPixelHeight = static_cast<int>(naturalCell.height);
        feed(*terminal, "\033_Ga=T,i=7,f=100,C=1;" + std::string(naturalImagePNG) + "\033\\");
        Ubiquitty::drainAsyncDecodes(*terminal);
        feed(*terminal, "\033[2;3H\033_Ga=p,i=7,p=2,C=1,x=2,y=3,w=12,h=7,X=3,Y=4\033\\");
        std::array<Ubiquitty::Image::ImageQuad, 2> natural {}, fitted {};
        REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), naturalCell, {}, backingScale, 0, natural) == 2);
        constexpr float fitScale = 0.5f;
        const Ubiquitty::FloatPoint origin { 5, 9 };
        REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(),
                    { naturalCell.width * fitScale, naturalCell.height * fitScale }, origin,
                    backingScale * fitScale, 0, fitted, {}, fitScale)
            == 2);
        for (std::size_t index = 0; index < natural.size(); ++index) {
            CHECK_EQ(fitted[index].minXInPixels, origin.x + natural[index].minXInPixels * fitScale);
            CHECK_EQ(fitted[index].maxXInPixels, origin.x + natural[index].maxXInPixels * fitScale);
            CHECK_EQ(fitted[index].minYInPixels, origin.y + natural[index].minYInPixels * fitScale);
            CHECK_EQ(fitted[index].maxYInPixels, origin.y + natural[index].maxYInPixels * fitScale);
            CHECK_EQ(fitted[index].u0, natural[index].u0);
            CHECK_EQ(fitted[index].u1, natural[index].u1);
            CHECK_EQ(fitted[index].v0, natural[index].v0);
            CHECK_EQ(fitted[index].v1, natural[index].v1);
            CHECK(fitted[index].filter == Ubiquitty::Image::ImageFilter::Smooth);
        }
        CHECK(terminal->columns == 80 && terminal->rows == 24);
    }
}

// A cell box within a pixel of the image's own size draws the image at that
// size, one texel a pixel, from the box's corner; one a whole pixel or more off
// is scaled to the box and filtered.
TEST(aBoxWithinAPixelOfTheSourceDrawsItAtItsOwnSize)
{
    auto terminal = makeTerm(24, 80);
    terminal->cellPixelWidth = 10;
    terminal->cellPixelHeight = 10;
    placeImage(*terminal, 4, 2);
    Ubiquitty::Image::ImageQuad quads[1];
    REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), { 10.1f, 10.2f }, {}, 1.0f, 0, quads) == 1);
    CHECK(quads[0].minXInPixels == 0.0f);
    CHECK(quads[0].maxXInPixels == 40.0f);
    CHECK(quads[0].maxYInPixels == 20.0f);
    CHECK(quads[0].filter == Ubiquitty::Image::ImageFilter::Nearest);

    REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), { 10.5f, 10.0f }, {}, 1.0f, 0, quads) == 1);
    CHECK(quads[0].maxXInPixels == 42.0f);
    CHECK(quads[0].filter == Ubiquitty::Image::ImageFilter::Smooth);

    // Twice the size on both axes is a whole-number enlargement.
    REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), { 20.0f, 20.0f }, {}, 1.0f, 0, quads) == 1);
    CHECK(quads[0].maxXInPixels == 80.0f);
    CHECK(quads[0].filter == Ubiquitty::Image::ImageFilter::Nearest);
}

// Position the cursor and drop a 1x1 kitty placement (one cell, cursor stays
// on it), used by the reflow-remap test to anchor images at known rows.
static void placeStay(Ubiquitty::Terminal& t, int row, int col)
{
    char cup[32];
    std::snprintf(cup, sizeof cup, "\033[%d;%dH", row + 1, col + 1);
    feed(t, cup);
    feed(t, "\033_Ga=T,f=24,s=1,v=1;/wAA\033\\");
}
static int anchorY(Ubiquitty::Terminal& t, int slot)
{
    const Ubiquitty::Image::TerminalImagePlacement* p = Ubiquitty::Image::placement(&t, slot);
    return p ? static_cast<int>(p->cursorY) : -9999;
}

// Inline-image placements re-anchor across a resize (VT100Display resize remap).
// A width rewrap moves a placement's physical row, so each anchors to its
// rewrap-stable logical-line index; all placements share one feed sweep. A
// height-only change never rewraps, so each placement rides the plain rowShift
// (count of rows crossing the visible/scrollback boundary). Expected anchor rows
// are hand-computed from the pre-resize layout and that shift.
TEST(testImageReflowRemap)
{
    // Height-only GROW (6 -> 10 rows): 3 history rows pull back onto the screen,
    // so every placement shifts DOWN by rowShift = +3.
    {
        auto t = makeTerm(6, 20);
        placeStay(*t, 0, 0); // slot0
        placeStay(*t, 2, 0); // slot1
        Ubiquitty::Screen::scrollViewportUp(t.get(), 3); // slot0 -> -3, slot1 -> -1, history = 3
        placeStay(*t, 4, 0); // slot2 (live)
        Ubiquitty::resize(*t, 10, 20);
        CHECK(anchorY(*t, 0) == 0); // -3 + 3
        CHECK(anchorY(*t, 1) == 2); // -1 + 3
        CHECK(anchorY(*t, 2) == 7); //  4 + 3
        t.reset();
    }

    // Height-only SHRINK (10 -> 6 rows), full screen of content: 4 top rows
    // spill into history, so every placement shifts up by rowShift = -4.
    {
        auto t = makeTerm(10, 20);
        for (int r = 0; r < 10; r++) {
            char cup[32];
            std::snprintf(cup, sizeof cup, "\033[%d;1H", r + 1);
            feed(*t, cup);
            feed(*t, "x");
        }
        placeStay(*t, 1, 0); // slot0
        placeStay(*t, 7, 0); // slot1
        Ubiquitty::resize(*t, 6, 20);
        CHECK(anchorY(*t, 0) == -3); // 1 - 4
        CHECK(anchorY(*t, 1) == 3); //  7 - 4
        t.reset();
    }

    // Width change, no wrapping, interleaved slot order (slot0 sits BELOW
    // slot1): the rewrap moves nothing, and the shared sweep must keep each
    // anchor with its own slot — a bookkeeping mixup would swap them.
    {
        auto t = makeTerm(8, 10);
        placeStay(*t, 5, 0); // slot0 row 5
        placeStay(*t, 2, 0); // slot1 row 2
        Ubiquitty::resize(*t, 8, 20);
        CHECK(anchorY(*t, 0) == 5);
        CHECK(anchorY(*t, 1) == 2);
        t.reset();
    }

    // Width GROW that actually rewraps: a 30-char logical line wraps to 2 rows
    // at width 20 and collapses to 1 row at width 40, so the image on the line
    // below rides up one row.
    {
        auto t = makeTerm(8, 20);
        feed(*t, "012345678901234567890123456789"); // 30 chars -> 20 + 10
        feed(*t, "\r\n");
        placeStay(*t, 2, 0); // slot0, logical line below the wrapped one
        Ubiquitty::resize(*t, 8, 40);
        CHECK(anchorY(*t, 0) == 1);
        t.reset();
    }

    // Width SHRINK with both placements already in history + interleaved slots:
    // short lines don't rewrap, so history (2 rows) folds back onto the screen
    // and each placement lands at its own logical row (history -> 0).
    {
        auto t = makeTerm(6, 20);
        placeStay(*t, 3, 0); // slot0 row 3
        placeStay(*t, 1, 0); // slot1 row 1
        Ubiquitty::Screen::scrollViewportUp(t.get(), 2); // slot0 -> 1, slot1 -> -1, history = 2
        Ubiquitty::resize(*t, 6, 10);
        CHECK(anchorY(*t, 0) == 3); // feed row 3 -> line 3 -> row 3
        CHECK(anchorY(*t, 1) == 1); // feed row 1 -> line 1 -> row 1
        t.reset();
    }
}

// Each of the three protocols decodes into one unified model (a kitty
// placement + an RGBA image) and resolves to a quad.

// Kitty graphics (APC): a=T transmit+display, 1x1 RGB pixel.
TEST(testKittyDecode)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033_Ga=T,f=24,s=1,v=1;/wAA\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 1 && img->height == 1);
    Ubiquitty::Image::ImageQuad q[4];
    CHECK(Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) }) == 1);
    t.reset();
}

// iTerm inline image (OSC 1337): base64 PNG, decoded + placed via the same model.
TEST(testITERMDecode)
{
    auto t = makeTerm(24, 80);
    feed(*t,
        "\033]1337;File=inline=1:"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEklEQVR4nGM4oaFxQkOD"
        "AUIBACEuBGGeECA5AAAAAElFTkSuQmCC\007");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 2 && img->height == 2);
    Ubiquitty::Image::ImageQuad q[4];
    CHECK(Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) }) == 1);
    t.reset();
}

// iTerm inline image (OSC 1337) carrying a baseline uncompressed TIFF — what
// `chafa -f iterm` emits. The portable container decoder must handle TIFF
// (not just PNG) so the headless / Linux path renders it. The payload is a
// 2x2 RGBA little-endian TIFF.
TEST(testITERMTiffDecode)
{
    auto t = makeTerm(24, 80);
    feed(*t,
        "\033]1337;File=inline=1:"
        "SUkqABgAAAD/AAD/AP8A/wAA////////CwAAAQQAAQAAAAIAAAABAQQAAQAAAAIAAAAC"
        "AQMABAAAAKIAAAADAQMAAQAAAAEAAAAGAQMAAQAAAAIAAAARAQQAAQAAAAgAAAAVAQMA"
        "AQAAAAQAAAAWAQQAAQAAAAIAAAAXAQQAAQAAABAAAAAcAQMAAQAAAAEAAABSAQMAAQAA"
        "AAIAAAAAAAAACAAIAAgACAA=\007");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 2 && img->height == 2);
    Ubiquitty::Image::ImageQuad q[4];
    CHECK(Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) }) == 1);
    t.reset();
}

// DEC sixel (DCS): "~" = six samples with a default 2:1 pixel aspect,
// stored as a 1x12 square-pixel image
// (one cell row), leaves the cursor on that row, and resolves to a quad.
TEST(testSixelDecode)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033Pq~\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 1 && img->height == 12);
    CHECK(t->screens.active->cursorY == 0); // single-cell-row image: cursor stays on it
    Ubiquitty::Image::ImageQuad q[4];
    CHECK(Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) }) == 1);
    t.reset();
}

// Unified post-image cursor: every backend leaves the cursor on the image's
// LAST row (anchor + cellRows - 1), so an emitter's trailing newline lands just
// below the image with no blank line. Checked for kitty (explicit r) and iTerm
// (explicit height); testSixelDecode covers sixel.
TEST(testImageCursorLandsOnLastRow)
{
    // kitty: r=4 anchored at row 0 -> cursor on the image's last row (3).
    auto t = makeTerm(24, 80);
    feed(*t, "\033_Ga=T,f=24,s=2,v=2,c=4,r=4;AAAAAAAAAAAAAAAA\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    CHECK(t->screens.active->cursorY == 3);
    t.reset();

    // iTerm: height=4 cells -> cursor on the image's last row (3).
    auto t2 = makeTerm(24, 80);
    feed(*t2,
        "\033]1337;File=inline=1;width=4;height=4:"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEklEQVR4nGM4oaFxQkOD"
        "AUIBACEuBGGeECA5AAAAAElFTkSuQmCC\007");
    CHECK(Ubiquitty::Image::placementCount(t2.get()) == 1);
    CHECK(t2->screens.active->cursorY == 3);
    t2.reset();
}

// A DCS ends only with ST (ECMA-48 8.3.27): BEL inside a sixel image rings and
// the image goes on until ST places it.
TEST(testSixelEndsOnlyWithTheStringTerminator)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033Pq~\007");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 0);
    feed(*t, "~\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    t.reset();
}

// Sixel P2=1 (transparent background): pixels no sixel bit ever paints stay
// transparent (alpha 0) instead of taking opaque color 0, so what is behind the
// unpainted pixels shows through. Feed reg-1=red, paint columns 0 and 5, leaving
// columns 1..4 unpainted in a 6x12 square-pixel image.
TEST(testSixelTransparency)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033P0;1;0q#1;2;100;0;0~!4?~\033\\");
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 6 && img->height == 12);
    if (img && img->format == 32) {
        // (x=0,y=0) painted red, opaque.
        CHECK(img->pixels[(0u * 6u + 0u) * 4u + 3u] == 255);
        CHECK(img->pixels[(0u * 6u + 0u) * 4u + 0u] == 255);
        // (x=2,y=0) never painted → transparent.
        CHECK(img->pixels[(0u * 6u + 2u) * 4u + 3u] == 0);
    }
    t.reset();
}

// Sixel wire bytes long enough that the fast loop's DCS payload run batcher
// engages: three 64-column bands of literal '~' data behind a palette entry.
static std::string sixelWire()
{
    std::string s = "\033P0;1;0q#1;2;100;0;0";
    for (int band = 0; band < 3; band++) {
        s.append(64, '~');
        s += '-';
    }
    s += "\033\\";
    return s;
}

static void feedSplit(Ubiquitty::Terminal& t, const std::string& s, const int* cuts, int cutCount)
{
    int pos = 0;
    for (int k = 0; k < cutCount; k++) {
        Ubiquitty::processBytes(&t, std::string_view(s.data() + pos, static_cast<std::size_t>(cuts[k] - pos)));
        pos = cuts[k];
    }
    Ubiquitty::processBytes(&t, std::string_view(s.data() + pos, static_cast<std::size_t>(static_cast<int>(s.size()) - pos)));
}

static bool sameSixelResult(Ubiquitty::Terminal& a, Ubiquitty::Terminal& b)
{
    if (Ubiquitty::Image::placementCount(&a) != Ubiquitty::Image::placementCount(&b))
        return false;
    const Ubiquitty::Image::TerminalImage* imageA = Ubiquitty::Image::stored(&a, 0);
    const Ubiquitty::Image::TerminalImage* imageB = Ubiquitty::Image::stored(&b, 0);
    if (!imageA || !imageB)
        return imageA == imageB;
    if (imageA->width != imageB->width || imageA->height != imageB->height || imageA->format != imageB->format)
        return false;
    if (a.screens.active->cursorY != b.screens.active->cursorY)
        return false;
    const std::size_t bytesPerPixel = imageA->format == 32 ? 4u : 3u;
    return std::memcmp(imageA->pixels, imageB->pixels, static_cast<std::size_t>(imageA->width) * imageA->height * bytesPerPixel) == 0;
}

// One sixel image fed whole, split at adversarial chunk boundaries, and
// per-byte must land the identical decode: the DCS payload run batcher and
// the per-byte DFA are the same machine.
TEST(testSixelChunkedFeedParity)
{
    const std::string s = sixelWire();
    const int escIndex = static_cast<int>(s.size()) - 2; // ESC of the closing ST

    auto whole = makeTerm(24, 80);
    Ubiquitty::processBytes(whole.get(), s);
    CHECK(Ubiquitty::Image::placementCount(whole.get()) == 1);

    // Cuts: mid payload run / just before the ESC / between ESC and backslash.
    auto split = makeTerm(24, 80);
    const int cuts[] = { 40, escIndex, escIndex + 1 };
    feedSplit(*split, s, cuts, 3);
    CHECK(sameSixelResult(*whole, *split));

    auto perByte = makeTerm(24, 80);
    for (std::size_t i = 0; i < s.size(); i++)
        Ubiquitty::processBytes(perByte.get(), std::string_view(s.data() + i, static_cast<std::size_t>(1)));
    CHECK(sameSixelResult(*whole, *perByte));

    whole.reset();
    split.reset();
    perByte.reset();
}

// Terminator handling through the batched path matches the per-byte DFA:
// Can / SUB abort the frame (no image; the parser is back in Ground so the
// next byte prints), BEL terminates like ST, and C1 ST (0x9C) is payload
// under UTF-8 input but a terminator under a legacy single-byte encoding.
// P2 decides what an unpainted pixel is: with P2=1 it is transparent, and
// otherwise it takes the background. Only the transparent case ever reads the
// decoder's written mask, so only that case allocates one — which makes P2 the
// switch between two different decodes and not a detail of one.
//
// The canvas crops to what was painted, so the sixel below paints the top row
// of a column and, after a graphics newline, the top row of the band beneath —
// leaving the five rows between them unpainted. The pixel one row down is then
// inside the image, and its alpha is what the mode decides.
TEST(testSixelTransparencyFollowsP2)
{
    struct ModeCase {
        const char* name;
        const char* introducer;
        unsigned char expectedAlpha;
    };
    // '@' is 0x40: bit 0 set, so the band's top row is painted and the five
    // below it are not.
    const ModeCase modes[] = {
        { "P2=1 leaves the unpainted pixel transparent", "\033P0;1;0q", 0 },
        { "P2=0 gives the unpainted pixel the background", "\033P0;0;0q", 255 },
        { "P2=2 gives the unpainted pixel the background", "\033P0;2;0q", 255 },
    };

    for (const ModeCase& mode : modes) {
        auto t = makeTerm(24, 80);
        std::string s = mode.introducer;
        s += "@-@"; // row 0 of this band, then row 0 of the next
        s += "\033\\";
        Ubiquitty::processBytes(t.get(), s);
        CHECK_MSG(Ubiquitty::Image::placementCount(t.get()) == 1, mode.name);

        const Ubiquitty::Image::TerminalImage* image = Ubiquitty::Image::stored(t.get(), 0);
        CHECK_MSG(image != nullptr && image->pixels != nullptr, mode.name);
        CHECK_MSG(image->format == 32, mode.name);
        CHECK_MSG(image->width >= 1 && image->height >= 2, mode.name);

        // Row 0 is painted whatever the mode says, so an opaque top row is what
        // makes the row below it a statement about P2 rather than about a decode
        // that produced nothing.
        const unsigned char* painted = image->pixels + 3;
        CHECK_MSG(*painted == 255, mode.name);
        const unsigned char* unpainted = image->pixels + static_cast<std::size_t>(image->width) * 2 * 4 + 3;
        CHECK_MSG(*unpainted == mode.expectedAlpha, mode.name);
        t.reset();
    }
}

TEST(testSixelTerminatorsMatchPerByteDfa)
{
    const char aborters[] = { 0x18, 0x1A };
    for (char aborter : aborters) {
        auto t = makeTerm(24, 80);
        std::string s = "\033Pq";
        s.append(40, '~');
        s += aborter;
        s += 'X';
        Ubiquitty::processBytes(t.get(), s);
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 0);
        // Cancel (0x18) abandons the string; substitute (0x1A) abandons it and
        // draws the error character, in a device control string as elsewhere
        // (VT220 reference, substitute).
        const int column = aborter == 0x1A ? 1 : 0;
        if (aborter == 0x1A)
            CHECK(t->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->codePoint == Ubiquitty::VT::substituteCharacter);
        CHECK(t->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column))->codePoint == 'X');
        t.reset();
    }

    // BEL mid-buffer rings and the frame goes on: only the ST after it places
    // the image, whether the run is batched or fed a byte at a time.
    {
        auto t = makeTerm(24, 80);
        std::string s = "\033Pq";
        s.append(40, '~');
        s += '\007';
        Ubiquitty::processBytes(t.get(), s);
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 0);
        Ubiquitty::processBytes(t.get(), std::string_view { "~\033\\" });
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
        t.reset();
    }

    // 0x9C with UTF-8 input is payload, not a terminator: the frame stays
    // open until the real ST arrives.
    {
        auto t = makeTerm(24, 80);
        std::string s = "\033Pq";
        s.append(20, '~');
        s += static_cast<char>(0x9C);
        s.append(20, '~');
        Ubiquitty::processBytes(t.get(), s);
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 0);
        feed(*t, "\033\\");
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
        t.reset();
    }

    // 0x9C under a legacy encoding is C1 ST: it terminates the frame.
    {
        auto t = makeTerm(24, 80);
        Ubiquitty::Unicode::encReset(&t->parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
        std::string s = "\033Pq";
        s.append(40, '~');
        s += static_cast<char>(0x9C);
        Ubiquitty::processBytes(t.get(), s);
        CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
        t.reset();
    }
}

// ED 2 (erase display) drops landed placements regardless of backend.
TEST(testImageClearedOnEd)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033[H\033Pq~\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    feed(*t, "\033[2J");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 0);
    t.reset();
}

// Explicit kitty c=/r= drive the rendered cell span (the chafa --scale stretch
// contract): a 2x2px image with c=4,r=2 must resolve to a 4-column x 2-row quad,
// not the image's native pixel size.
TEST(testKittyCrCellSpan)
{
    auto t = makeTerm(24, 80);
    // f=24 RGB, 2x2 = 12 bytes; "AAAA"... = 16 base64 chars = 12 zero bytes.
    feed(*t, "\033_Ga=T,f=24,s=2,v=2,c=4,r=2;AAAAAAAAAAAAAAAA\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    Ubiquitty::Image::ImageQuad q[4];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].maxXInPixels == 40.0f); // c=4 * 10px
        CHECK(q[0].maxYInPixels == 40.0f); // r=2 * 20px
    }
    t.reset();
}

// chafa's real chunking terminates the transfer with an EMPTY final m=0 chunk
// (ESC_Gm=0 ESC\, no payload) rather than carrying data on the m=0 chunk. The
// transfer must still complete + place the image; otherwise the image silently
// never renders and the cursor never advances.
TEST(testKittyChunkedEmptyFinal)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033_Ga=T,f=24,s=2,v=2,c=4,r=2,m=1\033\\"); // opening control, no payload
    feed(*t, "\033_Gm=1;AAAAAAAA\033\\"); // 8 base64 chars
    feed(*t, "\033_Gm=1;AAAAAAAA\033\\"); // 8 more — 16 total = 12 bytes (2x2 RGB)
    feed(*t, "\033_Gm=0\033\\"); // EMPTY terminator chunk (chafa-style)
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->width == 2 && img->height == 2);
    t.reset();
}

// Same, but chunked (m=1 … m=0) the way chafa transmits — c/r ride the opening
// chunk control and must survive the chunk merge into the placement.
TEST(testKittyCrChunked)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033_Ga=T,f=24,s=2,v=2,c=4,r=2,m=1\033\\"); // opening control, no payload
    feed(*t, "\033_Gm=1;AAAAAAAA\033\\"); // 8 base64 chars
    feed(*t, "\033_Gm=0;AAAAAAAA\033\\"); // final chunk
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 1);
    Ubiquitty::Image::ImageQuad q[4];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(4) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].maxXInPixels == 40.0f); // c=4 survives the chunk merge
        CHECK(q[0].maxYInPixels == 40.0f); // r=2
    }
    t.reset();
}

// iTerm2 OSC 1337 sizing (Source/Core/Image/TerminalImage.cpp inlineImageExtent),
// against iTerm2's VT100InlineImageHelper: units (cells / device px / %), an
// automatic axis derived from the other, preserveAspectRatio fit-vs-stretch,
// and narrowing to the columns left. Cells 10x20 px, grid 80x24, image 100x100.
TEST(testInlineImageExtent)
{
    using Ubiquitty::IntSize;
    using Ubiquitty::Image::inlineImageExtent;
    const IntSize cell { 10, 20 };
    const Ubiquitty::GridSize grid { 80, 24 };
    const IntSize square { 100, 100 };
    const auto check = [](const Ubiquitty::Image::InlineImageExtent& extent, IntSize covered, IntSize placement, IntSize drawn) {
        CHECK_EQ(extent.coveredCells.width, covered.width);
        CHECK_EQ(extent.coveredCells.height, covered.height);
        CHECK_EQ(extent.placementCells.width, placement.width);
        CHECK_EQ(extent.placementCells.height, placement.height);
        CHECK_EQ(extent.drawnPixels.width, drawn.width);
        CHECK_EQ(extent.drawnPixels.height, drawn.height);
    };
    // Cells alone: the renderer sizes the box in its own cells.
    check(inlineImageExtent("10", "", "", square, cell, grid, 0), { 10, 5 }, { 10, 0 }, {});
    check(inlineImageExtent("50%", "", "", square, cell, grid, 0), { 40, 20 }, { 40, 0 }, {});
    check(inlineImageExtent("", "50%", "", square, cell, grid, 0), { 24, 12 }, { 0, 12 }, {});
    for (const auto* preserve : { "", "1" }) {
        check(inlineImageExtent("10", "10", preserve, square, cell, grid, 0), { 10, 10 }, { 10, 0 }, {});
        check(inlineImageExtent("10", "1", preserve, square, cell, grid, 0), { 10, 1 }, { 0, 1 }, {});
    }
    check(inlineImageExtent("10", "10", "0", square, cell, grid, 0), { 10, 10 }, { 10, 10 }, {});
    // Device pixels, named or natural: drawn at exactly that size.
    check(inlineImageExtent("95px", "", "", square, cell, grid, 0), { 10, 5 }, { 10, 5 }, { 95, 95 });
    check(inlineImageExtent("", "", "", square, cell, grid, 0), { 10, 5 }, { 10, 5 }, { 100, 100 });
    check(inlineImageExtent("60px", "30px", "", square, cell, grid, 0), { 6, 2 }, { 6, 2 }, { 30, 30 });
    check(inlineImageExtent("60px", "30px", "0", square, cell, grid, 0), { 6, 2 }, { 6, 2 }, { 60, 30 });
    check(inlineImageExtent("60px", "2", "0", square, cell, grid, 0), { 6, 2 }, { 6, 2 }, { 60, 40 });
    // The snapshot cases' 24px box of a 24x24 image is 24 device pixels on every
    // arm's cell: 12x30 (CoreText and FreeType at 2x), 16x32 (Unifont), 6x15 (1x).
    for (const IntSize arm : { IntSize { 12, 30 }, IntSize { 16, 32 }, IntSize { 6, 15 } }) {
        const auto extent = inlineImageExtent("24px", "24px", "", { 24, 24 }, arm, { 14, 5 }, 0);
        CHECK_EQ(extent.drawnPixels.width, 24);
        CHECK_EQ(extent.drawnPixels.height, 24);
        CHECK_EQ(extent.coveredCells.width, (24 + arm.width - 1) / arm.width);
        CHECK_EQ(extent.coveredCells.height, (24 + arm.height - 1) / arm.height);
    }
    // Wider than the columns left: narrowed to them, the box restated in cells.
    check(inlineImageExtent("40px", "40px", "", { 40, 40 }, cell, grid, 78), { 2, 1 }, { 2, 1 }, { 20, 20 });
    // A hostile length saturates and narrows to the grid instead of wrapping.
    const auto hostile = inlineImageExtent("999999999999999px", "", "", square, cell, grid, 0);
    CHECK_EQ(hostile.coveredCells.width, 80);
    CHECK(hostile.drawnPixels.width <= 800 && hostile.drawnPixels.height <= 255 * 20);
}

TEST(testImageUploadStagingPool)
{
    using Ubiquitty::Rendering::acquireImageUploadStaging;
    using Ubiquitty::Rendering::ImageUploadStaging;

    std::vector<std::shared_ptr<ImageUploadStaging>> pool;
    std::shared_ptr<ImageUploadStaging> first = acquireImageUploadStaging(pool);
    CHECK(pool.size() == 1);
    first->sourcePixels.resize(4096);
    first->sourcePixels[0] = 0x11;
    first->stamp = 1;
    const std::size_t firstCapacity = first->sourcePixels.capacity();

    std::shared_ptr<ImageUploadStaging> snapshotPin = first;
    first.reset();
    std::shared_ptr<ImageUploadStaging> second = acquireImageUploadStaging(pool);
    CHECK(pool.size() == 2);
    CHECK(second != snapshotPin);
    second->sourcePixels.resize(4096);
    second->sourcePixels[0] = 0x22;
    second->stamp = 2;
    CHECK(snapshotPin->stamp == 1 && snapshotPin->sourcePixels[0] == 0x11);

    snapshotPin.reset();
    std::shared_ptr<ImageUploadStaging> reused = acquireImageUploadStaging(pool);
    CHECK(reused == pool[0]);
    reused->sourcePixels.resize(2048);
    CHECK(reused->sourcePixels.capacity() == firstCapacity);
}

TEST(testLiveViewAnchorsAtOriginAndFollowsTheScrollback)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // cell = 10x20 device px; grid 24x80 at origin (0,0); scale 1.
    auto t = makeTerm(24, 80);
    placeImage(*t, 4, 2); // 4 columns x 2 rows

    // Live view (viewOffset 0): anchored at (0,0), sized to the 4x2 cell span.
    n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 0.0f);
        CHECK(q[0].minYInPixels == 0.0f);
        CHECK(q[0].maxXInPixels == 40.0f); // 4 columns * 10px
        CHECK(q[0].maxYInPixels == 40.0f); // 2 rows * 20px
        CHECK(q[0].u0 == 0.0f && q[0].v0 == 0.0f);
        CHECK(q[0].u1 == 1.0f && q[0].v1 == 1.0f);
        CHECK(q[0].straightAlpha);
    }

    // Scrolling back by 3 rows shifts the image DOWN 3 cell-rows.
    n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 3, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1)
        CHECK(q[0].minYInPixels == 60.0f); // (cursorY 0 + 3) * 20px

    t.reset();
}

// The grid origin is an offset in device pixels, and its two components are not
// interchangeable. Every other case here passes (0,0) or a fullscreen fit that
// ignores the origin by design, so a quad placed at originY horizontally would
// satisfy all of them. This one is asymmetric in both axes and in the cell size,
// so each of the four terms lands somewhere only its own input can reach.
TEST(testGridOriginOffsetsEachAxisSeparately)
{
    Ubiquitty::Image::ImageQuad q[8];

    auto t = makeTerm(24, 80);
    placeImage(*t, 4, 2); // 4 columns x 2 rows

    const int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 7.0f, 13.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 7.0f); // originX, not originY
        CHECK(q[0].minYInPixels == 13.0f); // originY, not originX
        CHECK(q[0].maxXInPixels == 47.0f); // originX + 4 columns * 10px
        CHECK(q[0].maxYInPixels == 53.0f); // originY + 2 rows * 20px
    }
    t.reset();
}

TEST(testCellSpanSizingScalesWithColumnsAndRows)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // Cell-span sizing scales with c/r: 8x4 cells doubles the rect (the
    // chafa --scale stretch contract).
    auto t2 = makeTerm(24, 80);
    placeImage(*t2, 8, 4);
    n = Ubiquitty::Rendering::collectImageQuads(t2.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].maxXInPixels == 80.0f); // 8 columns * 10px
        CHECK(q[0].maxYInPixels == 80.0f); // 4 rows * 20px
    }
    t2.reset();
}

TEST(testFullscreenContainFitFillsTheDrawableNotTheGrid)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // DECSET 7402 fullscreen content: ContainFit fits the source into the FULL
    // DRAWABLE, not the cell grid — edge-to-edge on the long axis with only the
    // symmetric aspect letterbox, no padding / sub-cell-remainder borders. 40x20
    // source (2:1) into a 1000x600 drawable is width-constrained (1000/40=25 <
    // 600/20=30): 1000x500 centered at the drawable origin -> fills x [0,1000],
    // letterboxes y [50,550]. originX=8 padding is IGNORED (the grid fallback
    // would center in [8,808]x[0,480], the uneven-L/R + bottom borders this fixes).
    auto t3 = makeTerm(24, 80);
    placeImage(*t3, 4, 2);
    t3->modes.fullscreenContent = true;
    n = Ubiquitty::Rendering::collectImageQuads(t3.get(), { 10.0f, 20.0f }, { 8.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) }, { 1000.0f, 600.0f });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 0.0f); // edge-to-edge left, not originX 8
        CHECK(q[0].maxXInPixels == 1000.0f); // edge-to-edge right, not gridRight 808
        CHECK(q[0].minYInPixels == 50.0f); // symmetric top letterbox
        CHECK(q[0].maxYInPixels == 550.0f); // symmetric bottom letterbox
    }
    t3.reset();
}

TEST(testFullscreenStretchFitFillsTheDeclaredAspectBox)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // Stretch (t3): fit a box of the app-declared display aspect into the drawable and
    // STRETCH the source to fill it — independent X/Y, the source's pixel aspect discarded.
    // 40x20 source, ar=4:3 into 1000x600: the 4:3 box is height-constrained (600/3=200 <
    // 1000/4=250) -> 800x600 box centered -> x[100,900] y[0,600]. The 2:1 source stretches
    // into the 4:3 box (the DOS non-square-pixel correction).
    auto t4 = makeTerm(24, 80);
    placeImage(*t4, 4, 2);
    t4->modes.fullscreenContent = true;
    t4->fullscreenContentDisplay.fit = Ubiquitty::FullscreenContentFit::Stretch;
    t4->fullscreenContentDisplay.aspectWidth = 4;
    t4->fullscreenContentDisplay.aspectHeight = 3;
    n = Ubiquitty::Rendering::collectImageQuads(t4.get(), { 10.0f, 20.0f }, { 8.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) }, { 1000.0f, 600.0f });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 100.0f); // 4:3 box centered horizontally in 1000
        CHECK(q[0].maxXInPixels == 900.0f);
        CHECK(q[0].minYInPixels == 0.0f); // box fills the drawable height
        CHECK(q[0].maxYInPixels == 600.0f);
    }
    t4.reset();
}

TEST(testFullscreenNativeFitUsesTheLargestIntegerScale)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // Native (t4): draw the source at the largest INTEGER scale that fits, centered,
    // unstretched. 40x20 into 1050x600: contain scale is 26.25 (1050/40), floored to 26x
    // -> 1040x520 centered -> x[5,1045] y[40,560]. The integer floor is what distinguishes
    // it from contain (which would be 1050x525 at the fractional 26.25x).
    auto t5 = makeTerm(24, 80);
    placeImage(*t5, 4, 2);
    t5->modes.fullscreenContent = true;
    t5->fullscreenContentDisplay.fit = Ubiquitty::FullscreenContentFit::Native;
    n = Ubiquitty::Rendering::collectImageQuads(t5.get(), { 10.0f, 20.0f }, { 8.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) }, { 1050.0f, 600.0f });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 5.0f); // 40*26=1040 centered in 1050
        CHECK(q[0].maxXInPixels == 1045.0f);
        CHECK(q[0].minYInPixels == 40.0f); // 20*26=520 centered in 600
        CHECK(q[0].maxYInPixels == 560.0f);
    }
    t5.reset();
}

TEST(testFullscreenScaleFitUsesTheExactMultiplier)
{
    Ubiquitty::Image::ImageQuad q[8];
    int n;

    // Scale (fit=scale): the app sets the EXACT source-px -> device-px multiplier, unstretched
    // + centered — no auto-fit. 40x20 source, scale=10 -> 400x200 centered in 1000x600 ->
    // x[300,700] y[200,400]. (Unlike native, which would pick the largest integer that fits.)
    auto t6 = makeTerm(24, 80);
    placeImage(*t6, 4, 2);
    t6->modes.fullscreenContent = true;
    t6->fullscreenContentDisplay.fit = Ubiquitty::FullscreenContentFit::Scale;
    t6->fullscreenContentDisplay.scaleMultiplier = 10;
    n = Ubiquitty::Rendering::collectImageQuads(t6.get(), { 10.0f, 20.0f }, { 8.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) }, { 1000.0f, 600.0f });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 300.0f); // 40*10=400 centered in 1000
        CHECK(q[0].maxXInPixels == 700.0f);
        CHECK(q[0].minYInPixels == 200.0f); // 20*10=200 centered in 600
        CHECK(q[0].maxYInPixels == 400.0f);
    }
    t6.reset();
}
// How a quad's texels reach the pixels it covers: the filter every arm reads
// off the quad, the level of detail a filtered quad samples, and the mip chain
// the arms share. Nearest is for a whole-number enlargement only, 1:1
// included, where each texel covers an exact block; every other scale is
// filtered, because nearest at a fractional scale drops and doubles source
// rows, the stair-stepping a scaled curve shows. The chain's levels take the
// edge's colour into their transparent texels, so filtering across a
// straight-alpha edge has no dark fringe.

TEST(onlyAWholeNumberEnlargementIsDrawnNearest)
{
    CHECK(Ubiquitty::Image::imageFilterForScale(24.0f, 24.0f, 24, 24) == Ubiquitty::Image::ImageFilter::Nearest);
    CHECK(Ubiquitty::Image::imageFilterForScale(48.0f, 72.0f, 24, 24) == Ubiquitty::Image::ImageFilter::Nearest);
    // Within a hundredth of a pixel across the quad is the product.
    CHECK(Ubiquitty::Image::imageFilterForScale(48.004f, 48.0f, 24, 24) == Ubiquitty::Image::ImageFilter::Nearest);

    CHECK(Ubiquitty::Image::imageFilterForScale(24.08f, 24.0f, 24, 24) == Ubiquitty::Image::ImageFilter::Smooth);
    CHECK(Ubiquitty::Image::imageFilterForScale(36.0f, 48.0f, 24, 24) == Ubiquitty::Image::ImageFilter::Smooth);
    CHECK(Ubiquitty::Image::imageFilterForScale(228.0f, 228.0f, 256, 256) == Ubiquitty::Image::ImageFilter::Smooth);
    // A whole-number reduction is still a reduction: half the texels are lost
    // to nearest, so it is filtered too.
    CHECK(Ubiquitty::Image::imageFilterForScale(128.0f, 128.0f, 256, 256) == Ubiquitty::Image::ImageFilter::Smooth);
    CHECK(Ubiquitty::Image::imageFilterForScale(10.0f, 10.0f, 0, 10) == Ubiquitty::Image::ImageFilter::Smooth);
}

TEST(theLevelOfDetailIsTheLargerMinificationInOctaves)
{
    CHECK(Ubiquitty::Image::imageLevelOfDetail(256.0f, 256.0f, 256.0f, 256.0f) == 0.0f);
    CHECK(Ubiquitty::Image::imageLevelOfDetail(24.0f, 24.0f, 48.0f, 48.0f) == 0.0f);
    CHECK(std::fabs(Ubiquitty::Image::imageLevelOfDetail(256.0f, 256.0f, 128.0f, 128.0f) - 1.0f) < 1e-6f);
    CHECK(std::fabs(Ubiquitty::Image::imageLevelOfDetail(256.0f, 64.0f, 128.0f, 64.0f) - 1.0f) < 1e-6f);
    CHECK(std::fabs(Ubiquitty::Image::imageLevelOfDetail(256.0f, 256.0f, 228.0f, 228.0f) - std::log2(256.0f / 228.0f)) < 1e-6f);
}

TEST(theChainHalvesDownToOneTexel)
{
    CHECK_EQ(Ubiquitty::Image::imageMipLevelCount(1, 1), 1);
    CHECK_EQ(Ubiquitty::Image::imageMipLevelCount(256, 256), 9);
    // 5x3, 2x1, 1x1.
    CHECK_EQ(Ubiquitty::Image::imageMipLevelCount(5, 3), 3);
}

TEST(aTransparentTexelTakesTheNearestEdgesColourAndKeepsItsAlpha)
{
    // Red, then two transparent black texels: the nearer takes red from its
    // neighbour, and the farther from the nearer.
    std::array<std::uint8_t, 12> row { 200, 10, 20, 255, 0, 0, 0, 0, 0, 0, 0, 0 };
    Ubiquitty::Image::bleedIntoTransparentTexels(row.data(), 3, 1);
    CHECK_EQ(static_cast<int>(row[4]), 200);
    CHECK_EQ(static_cast<int>(row[5]), 10);
    CHECK_EQ(static_cast<int>(row[6]), 20);
    CHECK_EQ(static_cast<int>(row[7]), 0);
    CHECK_EQ(static_cast<int>(row[8]), 200);
    CHECK_EQ(static_cast<int>(row[11]), 0);

    // An image with nothing transparent is left as it is.
    std::array<std::uint8_t, 8> opaque { 1, 2, 3, 255, 4, 5, 6, 1 };
    const std::array<std::uint8_t, 8> before = opaque;
    Ubiquitty::Image::bleedIntoTransparentTexels(opaque.data(), 2, 1);
    CHECK(opaque == before);
}

TEST(aLevelWeighsColourByAlpha)
{
    // One opaque white texel among three transparent black ones: a quarter
    // covered, and white where it shows — not the grey a plain average makes.
    const std::array<std::uint8_t, 16> block { 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    std::array<std::uint8_t, 4> level {};
    Ubiquitty::Image::downsampleImageMipLevel(block.data(), 2, 2, level.data());
    CHECK_EQ(static_cast<int>(level[0]), 255);
    CHECK_EQ(static_cast<int>(level[1]), 255);
    CHECK_EQ(static_cast<int>(level[2]), 255);
    CHECK_EQ(static_cast<int>(level[3]), 64);

    // An odd level repeats its last row and column.
    const std::array<std::uint8_t, 12> column { 10, 10, 10, 255, 20, 20, 20, 255, 90, 90, 90, 255 };
    std::array<std::uint8_t, 4> half {};
    Ubiquitty::Image::downsampleImageMipLevel(column.data(), 1, 3, half.data());
    CHECK_EQ(static_cast<int>(half[0]), 15);
    CHECK_EQ(static_cast<int>(half[3]), 255);
}

UBIQUITTY_TEST_MAIN()
