// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <algorithm>

// test_kitty_graphics_transfer — the kitty image transfer, driven from the wire.
//
// Every byte here arrives as `ESC _ G <keys>;<payload> ESC \` from whatever is
// on the other end of the pty, so the payload length, the pixel dimensions and
// the image id are all values a remote program chose. The cases are mostly
// refusals for that reason, and each asserts what the terminal keeps rather than
// only what it answers: a transfer that fails has to leave no slot claimed and
// no pixels installed, or a later placement draws from a buffer nothing filled.
//
// The PNG path decodes on a worker and integrates at a later drain, so a
// transfer's effect is split across two calls. Both halves are driven: the
// submit through the parser, the integration through drainAsyncDecodes, and the
// abandonment through discardPendingDecodes.

#include "TerminalFixture.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/Graphics.h"
#include "Core/Rendering/PNGEncoder.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makePNG;
using UbiquittyTest::Kitty::makeRGBA;

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        if (t.images.stored[i].used && t.images.stored[i].pixels)
            ++n;
    }
    return n;
}

const Ubiquitty::Image::TerminalImage* imageWithID(const Ubiquitty::Terminal& t, std::uint32_t id)
{
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        if (t.images.stored[i].used && t.images.stored[i].imageID == id)
            return &t.images.stored[i];
    }
    return nullptr;
}

} // namespace

TEST(testAPNGArrivesOnTheWireAndIntegratesAtTheNextDrain)
{
    // The whole round trip, and the control every refusal below is measured
    // against. The decode runs on a worker and the pixels are installed by a
    // later drain, so the transfer's effect is split across two calls and
    // neither alone is the contract.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> png = makePNG(4, 3);
    CHECK_MSG(!png.empty(), "the fixture encoded a PNG");
    feed(t, apc("a=T,f=100,i=7", base64(png)));

    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 1, "one image after the drain");

    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 7);
    CHECK(image != nullptr);
    CHECK(image->width == 4 && image->height == 3);
    CHECK_MSG(image->pixels != nullptr, "the decoded buffer is installed");
    CHECK_MSG(image->pixelLength == 4 * 3 * 4, "four bytes a pixel");
}

TEST(testAPayloadThatIsNotAnImageIsRefusedAndClaimsNothing)
{
    // A PNG signature followed by bytes that are not a PNG is what a truncated
    // or hostile transfer looks like. The decode fails on the worker and the
    // completion has to answer rather than install: a slot marked used with no
    // pixels is a later placement reading nothing.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    std::vector<std::uint8_t> broken = makePNG(4, 3);
    CHECK(broken.size() > 40);
    for (std::size_t i = 16; i < broken.size(); ++i) // keep the signature, ruin the rest
        broken[i] = static_cast<std::uint8_t>(0xA5);
    feed(t, apc("a=T,f=100,i=9", base64(broken)));
    Ubiquitty::drainAsyncDecodes(t);

    CHECK_MSG(liveImageCount(t) == 0, "a failed decode installs nothing");
    CHECK_MSG(imageWithID(t, 9) == nullptr, "and claims no id");

    // Empty, and short-of-a-signature, take the same answer.
    feed(t, apc("a=T,f=100,i=10", ""));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 0);

    const std::vector<std::uint8_t> stub { 0x89, 0x50, 0x4E };
    feed(t, apc("a=T,f=100,i=11", base64(stub)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 0);

    // The control: a good PNG through the same terminal still lands, so the
    // refusals are about the payloads.
    feed(t, apc("a=T,f=100,i=12", base64(makePNG(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 1);
}

// discardPendingDecodes is reached here, but the state it exists for is not.
//
// A transfer fed through the parser is integrated by the drain on the way out,
// so by the time any call returns there is nothing pending to discard. The
// synchronized-output window the ownership cases use does leave a job queued —
// sometimes: whether the drain runs before the parse stops depends on where the
// payload's bytes fall, so the same construction with a slightly different
// image integrates instead. A fixture that opens the window only for some
// payload sizes cannot carry an assertion about what discarding does.
//
// So what is asserted here is the part that holds whatever the queue contains,
// and the pending path is left unmeasured rather than pinned against a fixture
// that decides on its own whether the case is about anything. Reaching it needs
// a seam that queues a job without running it.
TEST(testDiscardingIsSafeWhateverTheQueueHolds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // On an empty queue, which is what teardown finds most of the time, and
    // repeatedly, which is what lets a caller invoke it without tracking
    // whether a decode is outstanding.
    Ubiquitty::Kitty::discardPendingDecodes(&t);
    Ubiquitty::Kitty::discardPendingDecodes(&t);
    CHECK(liveImageCount(t) == 0);

    // After a completed transfer it takes nothing back: an image already
    // installed is not pending, and discarding must not reach into the slots.
    feed(t, apc("a=T,f=100,i=21", base64(makePNG(4, 4))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 1, "the transfer landed");
    Ubiquitty::Kitty::discardPendingDecodes(&t);
    CHECK_MSG(liveImageCount(t) == 1, "a discard does not evict what is already installed");
    CHECK(imageWithID(t, 21) != nullptr);

    // And the queue is usable afterwards rather than left in a state that
    // swallows the next transfer.
    feed(t, apc("a=T,f=100,i=22", base64(makePNG(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 22) != nullptr, "the next transfer still lands");
}

TEST(testRestoringAnIdReplacesThePixelsRatherThanAccumulating)
{
    // A program redrawing under one id is the ordinary case, and the old
    // pixels have to go with the old placements — otherwise every redraw leaks
    // a buffer and the slot budget is spent by a program doing nothing wrong.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    feed(t, apc("a=T,f=100,i=31", base64(makePNG(4, 4))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 1);
    const Ubiquitty::Image::TerminalImage* first = imageWithID(t, 31);
    CHECK(first != nullptr && first->width == 4);
    const std::uint32_t firstGeneration = first->contentGeneration;

    feed(t, apc("a=T,f=100,i=31", base64(makePNG(6, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 1, "the id still names one image");

    const Ubiquitty::Image::TerminalImage* second = imageWithID(t, 31);
    CHECK(second != nullptr);
    CHECK_MSG(second->width == 6 && second->height == 2, "the new pixels replaced the old");
    CHECK_MSG(second->contentGeneration != firstGeneration, "and the content generation moved");
}

TEST(testARawPixelTransferCarriesItsOwnDimensions)
{
    // f=32 hands over pixels rather than a container, so the width and height
    // are the sender's word and the payload length is checked against them.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> rgba = makeRGBA(3, 2);
    feed(t, apc("a=T,f=32,s=3,v=2,i=41", base64(rgba)));
    Ubiquitty::drainAsyncDecodes(t);

    CHECK_MSG(liveImageCount(t) == 1, "raw pixels install");
    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 41);
    CHECK(image != nullptr);
    CHECK(image->width == 3 && image->height == 2);

    // Dimensions the payload cannot fill are a refusal rather than a read past
    // the end: this is the shape where a length and a geometry disagree.
    feed(t, apc("a=T,f=32,s=9999,v=9999,i=42", base64(rgba)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 42) == nullptr, "a geometry the payload cannot fill is refused");

    // Zero in either dimension is not an image.
    feed(t, apc("a=T,f=32,s=0,v=2,i=43", base64(rgba)));
    feed(t, apc("a=T,f=32,s=3,v=0,i=44", base64(rgba)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(imageWithID(t, 43) == nullptr);
    CHECK(imageWithID(t, 44) == nullptr);
}

TEST(testOmittedImageFormatMatchesExplicitRGBA)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> rgba = makeRGBA(3, 2);
    for (const char* format : { "f=32,", "" }) {
        feed(t, apc(std::string(format) + "s=3,v=2,i=41", base64(rgba)));
        const auto* image = imageWithID(t, 41);
        REQUIRE(image != nullptr);
        CHECK_EQ(image->format, 32);
        CHECK_EQ(image->width, 3);
        CHECK_EQ(image->height, 2);
        REQUIRE(image->pixelLength == rgba.size());
        REQUIRE(image->pixels != nullptr);
        CHECK(std::memcmp(image->pixels, rgba.data(), rgba.size()) == 0);
        CHECK_EQ(t.images.livePlacementCount, 0);

        char outputBuffer[512];
        const int replyLength = UbiquittyTest::drain(t, outputBuffer);
        REQUIRE(replyLength > 0);
        const std::string reply(outputBuffer, static_cast<std::size_t>(replyLength));
        CHECK_MSG(reply.find(";OK") != std::string::npos, reply.c_str());
        feed(t, apc("a=d,q=2,d=I,i=41", ""));
        CHECK(imageWithID(t, 41) == nullptr);
    }

    // Omitting f still requires four bytes per pixel; RGB needs f=24.
    const std::vector<std::uint8_t> rgb { 0x12, 0x34, 0x56 };
    feed(t, apc("s=1,v=1,i=42", base64(rgb)));
    CHECK(imageWithID(t, 42) == nullptr);
    feed(t, apc("f=24,s=1,v=1,i=43", base64(rgb)));
    const auto* image = imageWithID(t, 43);
    REQUIRE(image != nullptr);
    CHECK_EQ(image->format, 24);
    REQUIRE(image->pixelLength == rgb.size());
    REQUIRE(image->pixels != nullptr);
    CHECK(std::memcmp(image->pixels, rgb.data(), rgb.size()) == 0);
}

TEST(testChunkedImageRetainsTheDefaultRGBAFormat)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> rgba = makeRGBA(3, 2);
    const std::string payload = base64(rgba);
    feed(t, apc("q=2,m=1,s=3,v=2,i=12345", payload.substr(0, 12)));
    CHECK(t.images.kittyChunkActive);
    CHECK(imageWithID(t, 12345) == nullptr);
    feed(t, apc("q=2,m=1", payload.substr(12, 8)));
    CHECK(t.images.kittyChunkActive);
    CHECK(imageWithID(t, 12345) == nullptr);
    feed(t, apc("q=2", payload.substr(20)));

    CHECK(!t.images.kittyChunkActive);
    const auto* image = imageWithID(t, 12345);
    REQUIRE(image != nullptr);
    CHECK_EQ(image->format, 32);
    CHECK_EQ(image->width, 3);
    CHECK_EQ(image->height, 2);
    REQUIRE(image->pixelLength == rgba.size());
    REQUIRE(image->pixels != nullptr);
    CHECK(std::memcmp(image->pixels, rgba.data(), rgba.size()) == 0);
    char outputBuffer[512];
    CHECK_EQ(UbiquittyTest::drain(t, outputBuffer), 0);

    feed(t, apc("a=d,q=2,d=I,i=12345", ""));
    CHECK(imageWithID(t, 12345) == nullptr);
}

TEST(testRawPixelsOutliveReusedTransferBuffers)
{
    // An uncompressed transfer installs the buffer its payload decoded into,
    // with no copy in between. The APC buffer and the chunk accumulator are
    // reused by every later transfer, so the installed pixels have to be bytes
    // the image owns: a second chunked transfer, its deletion, and a
    // replacement under the first id must leave each image holding its own.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> firstPixels = makeRGBA(3, 2);
    const std::string firstPayload = base64(firstPixels);
    feed(t, apc("q=2,m=1,f=32,s=3,v=2,i=41", firstPayload.substr(0, 12)));
    feed(t, apc("q=2", firstPayload.substr(12)));
    const auto* firstImage = imageWithID(t, 41);
    REQUIRE(firstImage != nullptr);
    const std::uint32_t firstGeneration = firstImage->contentGeneration;

    std::vector<std::uint8_t> secondPixels = firstPixels;
    for (auto& byte : secondPixels)
        byte = static_cast<std::uint8_t>(byte ^ 0xFF);
    const std::string secondPayload = base64(secondPixels);
    feed(t, apc("q=2,m=1,f=32,s=3,v=2,i=42", secondPayload.substr(0, 12)));
    feed(t, apc("q=2", secondPayload.substr(12)));
    const auto* secondImage = imageWithID(t, 42);
    REQUIRE(secondImage != nullptr);
    REQUIRE(secondImage->pixels != nullptr);
    REQUIRE(secondImage->pixelLength == secondPixels.size());
    CHECK(std::memcmp(secondImage->pixels, secondPixels.data(), secondPixels.size()) == 0);
    feed(t, apc("a=d,q=2,d=I,i=42", ""));
    CHECK(imageWithID(t, 42) == nullptr);

    firstImage = imageWithID(t, 41);
    REQUIRE(firstImage != nullptr);
    CHECK_EQ(firstImage->contentGeneration, firstGeneration);
    REQUIRE(firstImage->pixels != nullptr);
    REQUIRE(firstImage->pixelLength == firstPixels.size());
    CHECK(std::memcmp(firstImage->pixels, firstPixels.data(), firstPixels.size()) == 0);

    feed(t, apc("q=2,f=32,s=3,v=2,i=41", secondPayload));
    const auto* replacement = imageWithID(t, 41);
    REQUIRE(replacement != nullptr);
    CHECK(replacement->contentGeneration != firstGeneration);
    REQUIRE(replacement->pixels != nullptr);
    REQUIRE(replacement->pixelLength == secondPixels.size());
    CHECK(std::memcmp(replacement->pixels, secondPixels.data(), secondPixels.size()) == 0);
    CHECK_EQ(liveImageCount(t), 1);
}

TEST(testAPayloadThatIsNotBase64IsRefused)
{
    // The payload is base64 on the wire, so every byte of it is a character a
    // remote program chose. A character outside the alphabet, and padding in a
    // position the encoding does not allow, both have to stop the transfer
    // rather than decode to something shorter than the sender described.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::string good = base64(makeRGBA(2, 2));
    for (const char* bad : { "!!!!", "AAA A", "A*BC", "AB=C", "=AAA", "A", "AB", "AAAAA" }) {
        const int before = liveImageCount(t);
        feed(t, apc("a=T,f=32,s=2,v=2,i=51", std::string(bad)));
        Ubiquitty::drainAsyncDecodes(t);
        CHECK_MSG(liveImageCount(t) == before, bad);
    }

    // The control: the same keys with a well-formed payload do install, so the
    // refusals are about the encoding rather than about the geometry.
    feed(t, apc("a=T,f=32,s=2,v=2,i=52", good));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(imageWithID(t, 52) != nullptr);
}

TEST(testEveryImageSlotSpentIsRefusedRatherThanOverwritten)
{
    // The slot table is finite and a program can fill it. Refusing the next
    // transfer is what keeps an in-use image from being replaced underneath a
    // placement that is still drawing it.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::string payload = base64(makeRGBA(2, 2));
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount + 4; ++i) {
        feed(t, apc("a=T,f=32,s=2,v=2,i=" + std::to_string(1000 + i), payload));
        Ubiquitty::drainAsyncDecodes(t);
    }

    CHECK_MSG(liveImageCount(t) <= Ubiquitty::Image::maxTerminalImageCount,
        "the table never holds more than it has slots");
    CHECK_MSG(liveImageCount(t) > 0, "and the transfers before the ceiling did land");

    // What the name claims, which the count alone cannot see: an image with a
    // placement drawing it is not the one replaced. Every slot here is
    // displayed — a=T places as it transmits — so the store has nothing to
    // give, and the refusal is the answer rather than a picture disappearing
    // from under the reader looking at it.
    int displayedBefore = 0;
    std::vector<std::uint32_t> identifiersBefore;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (const Ubiquitty::Image::TerminalImage* image = Ubiquitty::Image::stored(&t, i); image && image->used) {
            identifiersBefore.push_back(image->imageID);
            displayedBefore += image->displayed ? 1 : 0;
        }
    CHECK_MSG(displayedBefore > 0, "the fixture placed at least one image, or it proves nothing");

    feed(t, apc("a=T,f=32,s=2,v=2,i=9999", payload));
    Ubiquitty::drainAsyncDecodes(t);

    std::vector<std::uint32_t> identifiersAfter;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (const Ubiquitty::Image::TerminalImage* image = Ubiquitty::Image::stored(&t, i); image && image->used)
            identifiersAfter.push_back(image->imageID);
    for (const std::uint32_t identifier : identifiersBefore)
        CHECK_MSG(std::ranges::find(identifiersAfter, identifier) != identifiersAfter.end(),
            "an image a placement was drawing went away to make room");
}

TEST(testAUnicodePlacementClaimsNoGridCell)
{
    // U=1 asks for a placement the grid does not position: the image is placed
    // later by placeholder cells. It must not also take a grid placement, or
    // the image is drawn twice from two different geometries.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    feed(t, apc("a=T,f=32,s=2,v=2,i=61,U=1,c=2,r=1", base64(makeRGBA(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* virtualImage = imageWithID(t, 61);
    CHECK_MSG(virtualImage != nullptr, "the image is stored");
    // The claim the case is named for. Storage was asserted on both arms
    // before, which is true of a grid placement too — so the sentence about not
    // taking a cell was checked by nothing.
    CHECK(virtualImage->virtualPlacement);
    CHECK(virtualImage->virtualColumns == 2);
    CHECK(virtualImage->virtualRows == 1);
    CHECK_MSG(t.images.livePlacementCount == 0, "and claims no grid placement");

    // The same transfer without U=1 is the grid-placed case, so the two are
    // distinguishable rather than one shape asserted twice.
    feed(t, apc("a=T,f=32,s=2,v=2,i=62,c=2,r=1", base64(makeRGBA(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* gridImage = imageWithID(t, 62);
    CHECK(gridImage != nullptr);
    CHECK(!gridImage->virtualPlacement);
    CHECK_MSG(t.images.livePlacementCount == 1, "the grid arm does take one");
}

/// `a=p,U=1` turns an image already stored into a virtual placement.
///
/// U=1 was only ever asked for alongside a transmit, so the branch that converts
/// an existing image had never run — a placement action reaching an image the
/// table already holds is a different path from one that stores and places in a
/// single control.
TEST(testPlacingAStoredImageVirtuallyTakesNoGridCell)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    char outputBuffer[512];

    feed(t, apc("a=t,f=32,s=2,v=2,i=64", base64(makeRGBA(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 64) != nullptr, "stored without being placed");
    CHECK(t.images.livePlacementCount == 0);
    CHECK(!imageWithID(t, 64)->virtualPlacement);

    feed(t, apc("a=p,i=64,U=1,c=3,r=2", ""));
    Ubiquitty::drainAsyncDecodes(t);
    const int replyLength = UbiquittyTest::drain(t, outputBuffer);
    const std::string reply(outputBuffer, static_cast<std::size_t>(replyLength > 0 ? replyLength : 0));
    CHECK_MSG(reply.find("OK") != std::string::npos, reply.c_str());

    const Ubiquitty::Image::TerminalImage* placed = imageWithID(t, 64);
    CHECK(placed != nullptr);
    CHECK(placed->virtualPlacement);
    CHECK(placed->virtualColumns == 3);
    CHECK(placed->virtualRows == 2);
    CHECK_MSG(t.images.livePlacementCount == 0, "a virtual placement takes no slot");
}

UBIQUITTY_TEST_MAIN()

/// A PNG query validates the image and answers, and stores nothing either way.
///
/// `a=q,f=100` runs the whole load pipeline — resolve the transport, decode the
/// PNG — and throws the pixels away with the answer. The existing query case
/// asks with `f=32`, so the decode branch this takes has never run.
///
/// The reply is what is asserted, because storage cannot tell the two apart: a
/// query stores nothing whether the image was good or malformed, so a case
/// checking only `liveImageCount` agrees with itself on both arms and reports
/// coverage it does not have.
TEST(testAPNGQueryValidatesWithoutStoring)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    char outputBuffer[512];

    feed(t, apc("a=q,f=100,i=71", base64(makePNG(3, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    const int okLength = UbiquittyTest::drain(t, outputBuffer);
    const std::string accepted(outputBuffer, static_cast<std::size_t>(okLength > 0 ? okLength : 0));
    CHECK(okLength > 0);
    CHECK_MSG(accepted.find("OK") != std::string::npos, accepted.c_str());
    CHECK_MSG(liveImageCount(t) == 0, "a query decodes and keeps nothing");
    CHECK(imageWithID(t, 71) == nullptr);

    // A PNG signature over bytes that are not one. The phrase is asserted whole:
    // several refusals begin EINVAL, and the code alone cannot say which arrived.
    std::vector<std::uint8_t> broken = makePNG(3, 2);
    for (std::size_t i = 16; i < broken.size(); ++i)
        broken[i] = static_cast<std::uint8_t>(0xA5);
    feed(t, apc("a=q,f=100,i=72", base64(broken)));
    Ubiquitty::drainAsyncDecodes(t);
    const int badLength = UbiquittyTest::drain(t, outputBuffer);
    const std::string refused(outputBuffer, static_cast<std::size_t>(badLength > 0 ? badLength : 0));
    CHECK(badLength > 0);
    CHECK_MSG(refused.find("EINVAL:invalid image data") != std::string::npos, refused.c_str());
    CHECK(liveImageCount(t) == 0);

    // No payload and no transport is the id-existence probe, which answers from
    // the table rather than from a decode.
    feed(t, apc("a=q,f=100,i=73", ""));
    Ubiquitty::drainAsyncDecodes(t);
    const int absentLength = UbiquittyTest::drain(t, outputBuffer);
    const std::string absent(outputBuffer, static_cast<std::size_t>(absentLength > 0 ? absentLength : 0));
    CHECK(absentLength > 0);
    CHECK_MSG(absent.find("ENOENT") != std::string::npos, absent.c_str());
}
