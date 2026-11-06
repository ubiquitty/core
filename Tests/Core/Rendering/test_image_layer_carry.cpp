// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What a frame draws when its image layer comes back empty.
//
// Two situations produce an empty layer and they mean opposite things: a frame
// that could not resolve has no answer yet, and a frame that resolved to
// nothing has one. Treating the second as the first pins a departed picture to
// the pixels it last occupied, which a session that scrolls through several
// images sees as a stuck copy of each.

#include "Harness.h"

#include "Core/Rendering/ImageLayerCarry.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdlib>
#include <cstring>

using Ubiquitty::Rendering::ImageLayerAction;
using Ubiquitty::Rendering::imageLayerAction;
using Ubiquitty::Rendering::ImageLayerFrame;

// The ordinary frame: placements resolved into quads, and those quads are what
// the frame draws.
TEST(testAResolvedLayerIsDrawn)
{
    ImageLayerFrame frame;
    frame.hasPlacements = true;
    frame.canResolve = true;
    frame.resolvedPlacements = 3;
    frame.drawableQuads = 3;
    CHECK(imageLayerAction(frame) == ImageLayerAction::UseResolved);
}

// An image scrolled above the viewport is still a live placement — it is
// reachable by scrolling back — but it resolves to no quads, and a frame that
// asked and was told "nothing on screen" draws nothing. Carrying here is the
// duplication: the picture stays where it was while the text moves out from
// under it.
TEST(testAResolveThatFoundNothingDrawsNothing)
{
    ImageLayerFrame frame;
    frame.hasPlacements = true;
    frame.canResolve = true;
    frame.resolvedPlacements = 0; // every placement clipped outside the viewport
    frame.drawableQuads = 0;
    frame.hasCarriedLayer = true; // a previous layer exists and must not be used
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// A frame that cannot resolve has no answer: the cell geometry is not primed
// yet, or there is no store to resolve against. The picture has not gone
// anywhere, so the previous layer bridges the gap rather than blanking.
TEST(testAFrameThatCannotResolveCarriesThePreviousLayer)
{
    ImageLayerFrame frame;
    frame.hasPlacements = true;
    frame.canResolve = false;
    frame.hasCarriedLayer = true;
    CHECK(imageLayerAction(frame) == ImageLayerAction::CarryPrevious);

    // With nothing to carry there is nothing to draw either.
    frame.hasCarriedLayer = false;
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// No placements at all is a delete, and the layer goes with it.
TEST(testNoPlacementsDrawsNothing)
{
    ImageLayerFrame frame;
    frame.hasPlacements = false;
    frame.canResolve = true;
    frame.hasCarriedLayer = true;
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// Except during a live resize, where an image application deleting and
// re-placing every frame shows up as a momentary zero. Holding the last layer
// across it keeps the picture steady while the window is dragged.
TEST(testALiveResizeHoldsThePictureAcrossADeleteAndReplaceGap)
{
    ImageLayerFrame frame;
    frame.hasPlacements = false;
    frame.canResolve = true;
    frame.isHoldingForResize = true;
    frame.hasCarriedLayer = true;
    CHECK(imageLayerAction(frame) == ImageLayerAction::CarryPrevious);

    // The hold does not invent a layer that was never there.
    frame.hasCarriedLayer = false;
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// The hold covers the delete-and-replace gap, not a placement that scrolled
// away: a resize with live placements that resolve to nothing still draws
// nothing, because the picture really has left the viewport.
TEST(testTheResizeHoldDoesNotPinAScrolledAwayImage)
{
    ImageLayerFrame frame;
    frame.hasPlacements = true;
    frame.canResolve = true;
    frame.resolvedPlacements = 0;
    frame.drawableQuads = 0;
    frame.isHoldingForResize = true;
    frame.hasCarriedLayer = true;
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// A picture the model puts on the viewport that the frontend cannot draw yet —
// its texture upload is still in flight — is a gap, not a departure. Blanking
// there flickers the image out and back on the frame the upload lands, which is
// what "shows and then disappears" looks like from the outside.
TEST(testAnImageWaitingOnItsTextureHoldsThePreviousLayer)
{
    ImageLayerFrame frame;
    frame.hasPlacements = true;
    frame.canResolve = true;
    frame.resolvedPlacements = 1; // the model says it is on screen
    frame.drawableQuads = 0; // the texture is not ready
    frame.hasCarriedLayer = true;
    CHECK(imageLayerAction(frame) == ImageLayerAction::CarryPrevious);

    // The very first frame of an image has nothing to carry, so it waits one
    // frame rather than drawing something that was never there.
    frame.hasCarriedLayer = false;
    CHECK(imageLayerAction(frame) == ImageLayerAction::DrawNothing);
}

// The model-side fact the frame is described with, asked of the model rather
// than of a store: a placement record is one way to place an image and a kitty
// placeholder run is another, and a frame that asked only one would read the
// other's picture as a delete.
TEST(testTheModelSaysWhetherAnythingIsPlaced)
{
    using Ubiquitty::Rendering::hasImagePlacements;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    CHECK_MSG(!hasImagePlacements(&t, 0), "a fresh terminal places nothing");

    // 40x20 pixels into a 4x2 cell box; the store takes the buffer's ownership.
    auto* pixels = static_cast<unsigned char*>(std::malloc(40 * 20 * 4));
    CHECK(pixels != nullptr);
    std::memset(pixels, 0xFF, 40 * 20 * 4);
    CHECK(Ubiquitty::Image::storeRGBAImage(&t, pixels, 40, 20, 4, 2));
    CHECK_MSG(hasImagePlacements(&t, 0), "a stored image is a placement");

    // ED 2 is the whole-screen clear that takes images with it, which is the
    // delete an image application's redraw begins with.
    Ubiquitty::processBytes(&t, "\033[2J");
    CHECK_MSG(!hasImagePlacements(&t, 0), "a whole-screen clear places nothing again");
}

UBIQUITTY_TEST_MAIN()
