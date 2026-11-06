// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Image placements are screen-aware: a picture anchored on one screen buffer
// survives a round trip through the other, is hidden while that other screen
// is active, and is untouched by scrolling or erasing done there.
//
// Before this suite existed, DECSET 1049h's erase-in-display swept every
// placement regardless of which screen it was anchored on, so viewing any
// fullscreen program destroyed every inline image on the primary screen.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/TerminalOperations.h"

#include <cstdlib>
#include <cstring>
#include <memory>

using UbiquittyTest::feed;

// Where an image is drawn lives in the placement table alone: a stored image
// carries no anchor or placement ID of its own for a renderer to read instead.
template<typename Image>
concept CarriesItsOwnAnchor = requires(Image image) { image.cursorX; } || requires(Image image) { image.cursorY; };
template<typename Image>
concept CarriesAPlacementID = requires(Image image) { image.placementID; };
static_assert(!CarriesItsOwnAnchor<Ubiquitty::Image::TerminalImage>);
static_assert(!CarriesAPlacementID<Ubiquitty::Image::TerminalImage>);
static_assert(CarriesItsOwnAnchor<Ubiquitty::Image::TerminalImagePlacement> && CarriesAPlacementID<Ubiquitty::Image::TerminalImagePlacement>);

static std::unique_ptr<Ubiquitty::Terminal> makeTerm(int rows, int columns)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

// Drop a 40x20px RGBA image spanning 4x2 cells at the cursor via the shared
// store — the same path sixel and OSC 1337 take.
static void placeStoredImage(Ubiquitty::Terminal& t)
{
    unsigned char* px = static_cast<unsigned char*>(std::malloc(40u * 20u * 4u));
    CHECK(px != nullptr);
    if (!px)
        return;
    std::memset(px, 0xFF, 40u * 20u * 4u);
    // The store takes ownership on success; on refusal the buffer is still
    // this function's to release, and a test that leaks it hides the refusal
    // from the sanitiser that would otherwise report it.
    if (!Ubiquitty::Image::storeRGBAImage(&t, px, 40, 20, 4, 2)) {
        std::free(px);
        CHECK(false);
    }
}

static int quadCount(const std::unique_ptr<Ubiquitty::Terminal>& t, int viewOffset = 0)
{
    Ubiquitty::Image::ImageQuad q[8];
    return Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, viewOffset,
        { q, static_cast<std::size_t>(8) });
}

static int livePlacements(const std::unique_ptr<Ubiquitty::Terminal>& t)
{
    return Ubiquitty::Image::placementCount(t.get());
}

TEST(testPrimaryImageSurvivesAlternateScreenRoundTrip)
{
    auto t = makeTerm(24, 80);
    placeStoredImage(*t);
    CHECK(quadCount(t) == 1);

    // Enter the alternate screen: the primary image is hidden but alive.
    feed(*t, "\033[?1049h");
    CHECK(quadCount(t) == 0);
    CHECK(livePlacements(t) == 1);

    // Leave: the picture comes back exactly as it was.
    feed(*t, "\033[?1049l");
    CHECK(livePlacements(t) == 1);
    CHECK(quadCount(t) == 1);
}

TEST(testAlternateScreenHidesPrimaryAndShowsItsOwn)
{
    auto t = makeTerm(24, 80);
    placeStoredImage(*t);

    feed(*t, "\033[?1049h");
    feed(*t, "\033_Ga=T,t=d,f=24,s=1,v=1,i=77;/wAA\033\\");
    // The alternate screen shows its own picture, never the primary's.
    CHECK(quadCount(t) == 1);

    feed(*t, "\033[?1049l");
    // Back on primary: only the stored primary image resolves.
    CHECK(quadCount(t) == 1);
}

TEST(testEraseDisplayOnAlternateScreenSparesPrimaryImage)
{
    auto t = makeTerm(24, 80);
    placeStoredImage(*t);

    feed(*t, "\033[?1049h");
    feed(*t, "\033[2J");
    feed(*t, "\033[?1049l");

    CHECK(livePlacements(t) == 1);
    CHECK(quadCount(t) == 1);
}

TEST(testScrollingOnAlternateScreenLeavesPrimaryAnchor)
{
    auto t = makeTerm(24, 80);
    feed(*t, "\033[3;4H"); // anchor away from the edges so scroll is observable
    placeStoredImage(*t);
    const auto* before = Ubiquitty::Image::placement(t.get(), 0);
    const int anchorBefore = before ? static_cast<int>(before->cursorY) : -9999;

    feed(*t, "\033[?1049h");
    for (int i = 0; i < 6; i++)
        feed(*t, "\r\n");
    feed(*t, "\033[?1049l");

    const auto* after = Ubiquitty::Image::placement(t.get(), 0);
    CHECK(after != nullptr);
    if (after)
        CHECK(static_cast<int>(after->cursorY) == anchorBefore);
    CHECK(quadCount(t) == 1);
}

UBIQUITTY_TEST_MAIN()
