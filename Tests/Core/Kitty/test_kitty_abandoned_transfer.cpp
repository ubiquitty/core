// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_kitty_abandoned_transfer — a chunked transfer whose sender went away.
//
// A program killed between an `m=1` block and its final block leaves the
// terminal holding an open chunk. The protocol gives a continuation block only
// `m` and `q`, so the next program's commands are recognisable as commands of
// their own: they name an action the open transfer did not, or carry the keys
// only a first block carries. Each case drives the abandoned opener and then
// asserts that the next program's command did what it says, rather than being
// appended to pixels nobody will finish sending.

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"

#include <cstdint>
#include <string>

namespace {

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makeRGBA;

int placementCount(const Ubiquitty::Terminal& t)
{
    int count = 0;
    for (const Ubiquitty::Image::TerminalImagePlacement& placement : t.images.placements) {
        if (placement.used)
            ++count;
    }
    return count;
}

const Ubiquitty::Image::TerminalImage* imageWithID(const Ubiquitty::Terminal& t, std::uint32_t id)
{
    for (const Ubiquitty::Image::TerminalImage& image : t.images.stored) {
        if (image.used && image.imageID == id)
            return &image;
    }
    return nullptr;
}

std::string pixels(int width, int height)
{
    return base64(makeRGBA(width, height));
}

/// The first block of a 32x32 transmit-and-display whose sender dies before
/// the rest: the shape the live-app harness's image scenario leaves behind when
/// it is interrupted between blocks.
void abandonTransfer(Ubiquitty::Terminal& t)
{
    const std::string whole = pixels(32, 32);
    feed(t, apc("a=T,f=32,s=32,v=32,i=100003,p=100003,c=4,r=2,C=1,q=2,m=1", whole.substr(0, whole.size() / 2)));
}

void prepare(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 10, 40, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
}

} // namespace

TEST(testADeleteAfterAnAbandonedTransferDeletes)
{
    // The images a killed program placed stay on the screen, and a delete is how
    // the next program, or the user, removes them. Taking the delete as the
    // final block of the dead transfer leaves every one of them drawn.
    Ubiquitty::Terminal t;
    prepare(t);
    feed(t, apc("a=T,f=32,s=2,v=2,i=7,c=2,r=1,C=1,q=2", pixels(2, 2)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(placementCount(t) == 1, "the control: a complete transmit places its image");

    abandonTransfer(t);
    feed(t, apc("a=d,d=A", ""));
    CHECK_MSG(placementCount(t) == 0, "a delete-all after an abandoned transfer removes every placement");
    CHECK_MSG(imageWithID(t, 7) == nullptr, "and the upper-case mode frees the image it unplaced");
}

TEST(testATransferAfterAnAbandonedOneIsStoredWhole)
{
    // The next program's first image carries its own format and size, which a
    // continuation block never does. Appending it to the dead transfer's half
    // payload stores neither, and it is refused under the dead sender's quiet
    // flag, so the program is never told.
    Ubiquitty::Terminal t;
    prepare(t);
    abandonTransfer(t);

    feed(t, apc("a=T,f=32,s=1,v=1,i=9,c=1,r=1,C=1", pixels(1, 1)));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 9);
    CHECK_MSG(image != nullptr, "a complete transmit after an abandoned transfer is stored");
    if (image) {
        CHECK(image->width == 1);
        CHECK(image->height == 1);
    }
    CHECK_MSG(imageWithID(t, 100003) == nullptr, "the abandoned transfer stores nothing");
    CHECK_MSG(!t.images.kittyChunkActive, "and no transfer is left open");
}

TEST(testAChunkedTransferAfterAnAbandonedOneAssemblesItsOwnBlocks)
{
    // The same program run again opens the same transfer with the same keys.
    // Its opener restarts the transfer, so its own continuations complete it
    // with its own pixels rather than after the dead run's half.
    Ubiquitty::Terminal t;
    prepare(t);
    abandonTransfer(t);

    const std::string whole = pixels(32, 32);
    const std::size_t half = whole.size() / 2;
    feed(t, apc("a=T,f=32,s=32,v=32,i=100003,p=100003,c=4,r=2,C=1,q=2,m=1", whole.substr(0, half)));
    feed(t, apc("m=0", whole.substr(half)));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 100003);
    CHECK_MSG(image != nullptr, "the restarted transfer completes");
    CHECK_MSG(placementCount(t) == 1, "and places its image once");
}

TEST(testAContinuationBlockStillContinues)
{
    // The control for the cases above: a block carrying only the keys a
    // continuation may carry still belongs to the open transfer, including the
    // `i` some clients restate on their final block.
    Ubiquitty::Terminal t;
    prepare(t);
    const std::string whole = pixels(4, 2);
    const std::size_t third = whole.size() / 3;
    feed(t, apc("a=t,f=32,s=4,v=2,i=61,m=1", whole.substr(0, third)));
    feed(t, apc("q=1,m=1", whole.substr(third, third)));
    feed(t, apc("i=61,m=0", whole.substr(2 * third)));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 61);
    CHECK_MSG(image != nullptr, "the continuation blocks complete the transfer");
    if (image) {
        CHECK(image->width == 4);
        CHECK(image->height == 2);
    }
}

UBIQUITTY_TEST_MAIN()
