// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_kitty_graphics_actions — the verbs a transfer is not.
//
// `a=` selects what an APC graphics block does: store, place an image already
// stored, delete, query, add an animation frame, or drive the animation. Only
// storing had ever been driven, so every other verb reached its function for the
// first time here — including the ones that free pixel buffers and the ones that
// write a reply back up the pty.
//
// Deletion is the half that matters most. A delete verb selects images by id, by
// number, by placement, or by screen position, and each mode decides whether the
// pixels go with the placement or stay for a later placement to draw. Freeing a
// buffer another placement still points at, or keeping one nothing can reach,
// are the two ways to get it wrong and they look identical from the reply.

#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/Graphics.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

void feed(Ubiquitty::Terminal& t, const std::string& s)
{
    Ubiquitty::processBytes(&t, s);
}

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        if (t.images.stored[i].used && t.images.stored[i].pixels)
            ++n;
    }
    return n;
}

int placementCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImagePlacementCount; ++i) {
        if (t.images.placements[i].used)
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

std::string base64(std::span<const std::uint8_t> bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t single = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += alphabet[(single >> 18) & 0x3F];
        out += alphabet[(single >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t pair = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += alphabet[(pair >> 18) & 0x3F];
        out += alphabet[(pair >> 12) & 0x3F];
        out += alphabet[(pair >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string apc(const std::string& keys, const std::string& payload = "")
{
    return "\033_G" + keys + ";" + payload + "\033\\";
}

std::vector<std::uint8_t> rgbaPixels(int width, int height)
{
    std::vector<std::uint8_t> v(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<std::uint8_t>(i & 0xFF);
    return v;
}

/// Stores one image under `id`, without displaying it.
void store(Ubiquitty::Terminal& t, std::uint32_t id, int width = 2, int height = 2)
{
    feed(t, apc("a=t,f=32,s=" + std::to_string(width) + ",v=" + std::to_string(height) + ",i=" + std::to_string(id), base64(rgbaPixels(width, height))));
    Ubiquitty::drainAsyncDecodes(t);
}

} // namespace

TEST(testAVerbTheProtocolDoesNotCarryIsRefused)
{
    // The action byte comes off the wire, so every value a byte can hold is one
    // a program may send. An unknown one answers rather than falling into
    // whichever branch is nearest.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    for (const char* verb : { "a=x", "a=Z", "a=0", "a=!" }) {
        feed(t, apc(std::string(verb) + ",f=32,s=2,v=2,i=1", base64(rgbaPixels(2, 2))));
        Ubiquitty::drainAsyncDecodes(t);
        CHECK_MSG(liveImageCount(t) == 0, verb);
    }

    // The control: the verb that does store still does, so the refusals are
    // about the byte rather than about this payload.
    store(t, 1);
    CHECK(liveImageCount(t) == 1);
}

TEST(testPlacingAnImageAlreadyStoredNeedsNoPayload)
{
    // `a=p` draws an image the terminal already holds, so a program that sent
    // the pixels once can place them repeatedly without resending. Placing an
    // id nothing stored must answer rather than allocate against nothing.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    store(t, 5);
    CHECK(liveImageCount(t) == 1);
    const int before = placementCount(t);

    feed(t, apc("a=p,i=5,c=2,r=1"));
    CHECK_MSG(placementCount(t) > before, "placing an existing image adds a placement");
    CHECK_MSG(liveImageCount(t) == 1, "and does not duplicate the image");

    // A second placement of the same image is another placement, not another
    // image: this is what a program tiling one bitmap does.
    const int afterFirst = placementCount(t);
    feed(t, apc("a=p,i=5,c=2,r=1"));
    CHECK(placementCount(t) > afterFirst);
    CHECK(liveImageCount(t) == 1);

    // An id nothing holds places nothing.
    const int afterTwo = placementCount(t);
    feed(t, apc("a=p,i=999,c=2,r=1"));
    CHECK_MSG(placementCount(t) == afterTwo, "an unknown id places nothing");
}

TEST(testDeletingByIdTakesThePlacementsWithIt)
{
    // The lower-case modes drop placements and keep the pixels for a later
    // placement; the upper-case ones free the pixels too. Getting that backwards
    // either leaks a buffer per redraw or frees one a placement still draws.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    store(t, 11);
    feed(t, apc("a=p,i=11,c=2,r=1"));
    CHECK(liveImageCount(t) == 1);
    CHECK(placementCount(t) > 0);

    // Lower case: the placement goes, the image stays placeable.
    feed(t, apc("a=d,d=i,i=11"));
    CHECK_MSG(placementCount(t) == 0, "the placement is gone");
    CHECK_MSG(imageWithID(t, 11) != nullptr, "the image is still stored");

    // Upper case: the image goes too.
    feed(t, apc("a=p,i=11,c=2,r=1"));
    CHECK(placementCount(t) > 0);
    feed(t, apc("a=d,d=I,i=11"));
    CHECK_MSG(placementCount(t) == 0, "the placement is gone");
    CHECK_MSG(imageWithID(t, 11) == nullptr, "and so are the pixels");
}

TEST(testDeletingEverythingLeavesNothingBehind)
{
    // `d=a` with nothing else selects every placement, and `d=A` every image.
    // A program clearing the screen sends one of these, so leaving a slot used
    // spends the table for the rest of the session.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    for (std::uint32_t id : { 21u, 22u, 23u }) {
        store(t, id);
        feed(t, apc("a=p,i=" + std::to_string(id) + ",c=2,r=1"));
    }
    CHECK(liveImageCount(t) == 3);
    CHECK(placementCount(t) >= 3);

    feed(t, apc("a=d,d=a"));
    CHECK_MSG(placementCount(t) == 0, "every placement is gone");
    CHECK_MSG(liveImageCount(t) == 3, "and every image is still stored");

    feed(t, apc("a=d,d=A"));
    CHECK_MSG(liveImageCount(t) == 0, "every image is gone too");

    // The default delete mode is the placement-only one, so an `a=d` with no
    // `d=` does not take the pixels.
    store(t, 24);
    feed(t, apc("a=p,i=24,c=2,r=1"));
    feed(t, apc("a=d"));
    CHECK(placementCount(t) == 0);
    CHECK_MSG(liveImageCount(t) == 1, "the default mode keeps the image");
}

TEST(testDeletingByNumberAndByPlacementSelectWhatTheyName)
{
    // The selectors are separate code paths and each is what a different client
    // uses: an image number for a program that never learned its assigned id,
    // a placement id for one drawing the same image in several places.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // By image number: the client never learned the id it was assigned, so it
    // addresses the image by the number it chose. Lower case keeps the pixels.
    feed(t, apc("a=t,f=32,s=2,v=2,I=77", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 1);
    feed(t, apc("a=p,I=77,c=2,r=1"));
    CHECK(placementCount(t) > 0);
    feed(t, apc("a=d,d=n,I=77"));
    CHECK_MSG(placementCount(t) == 0, "the placement goes");
    CHECK_MSG(liveImageCount(t) == 1, "and the lower-case mode keeps the pixels");

    // Upper case frees them.
    feed(t, apc("a=d,d=N,I=77"));
    CHECK_MSG(liveImageCount(t) == 0, "the upper-case mode frees the image data");

    // A number nothing carries selects nothing rather than the nearest image.
    store(t, 78);
    feed(t, apc("a=d,d=N,I=999"));
    CHECK_MSG(liveImageCount(t) == 1, "an unmatched number frees nothing");
    feed(t, apc("a=d,d=A"));

    // By placement id: only the named placement goes.
    store(t, 31);
    feed(t, apc("a=p,i=31,p=1,c=2,r=1"));
    feed(t, apc("a=p,i=31,p=2,c=2,r=1"));
    const int both = placementCount(t);
    CHECK(both >= 2);
    feed(t, apc("a=d,d=i,i=31,p=1"));
    CHECK_MSG(placementCount(t) < both, "the named placement is gone");
    CHECK_MSG(placementCount(t) > 0, "and the other one is not");
    CHECK_MSG(imageWithID(t, 31) != nullptr, "nor are the pixels");
}

TEST(testQueryAnswersWithoutStoringAnything)
{
    // `a=q` asks whether a transfer would be accepted. It must answer and store
    // nothing — a query that allocated a slot would let a client exhaust the
    // table by asking questions.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    feed(t, apc("a=q,f=32,s=2,v=2,i=41", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 0, "a query stores nothing");
    CHECK(imageWithID(t, 41) == nullptr);

    // Repeatedly, which is what a client probing support does.
    for (int i = 0; i < 8; ++i)
        feed(t, apc("a=q,f=32,s=2,v=2,i=42", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 0, "and cannot spend the table by asking");

    // A query naming a geometry no image can have is still a query rather than
    // an error that stores.
    feed(t, apc("a=q,f=32,s=0,v=0,i=43", ""));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 0);
}

TEST(testAnimationFramesAndControlReachTheirOwnPaths)
{
    // A frame is added against an image already stored, and the control verb
    // drives what is displayed. Both are verbs nothing had exercised, and both
    // take an id off the wire that may name nothing.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    store(t, 51, 2, 2);
    CHECK(liveImageCount(t) == 1);

    // A frame for a stored image.
    feed(t, apc("a=f,f=32,s=2,v=2,i=51", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 1, "a frame joins its image rather than becoming one");

    // A frame for an id nothing holds.
    feed(t, apc("a=f,f=32,s=2,v=2,i=999", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(liveImageCount(t) == 1, "a frame for an unknown image stores nothing");

    // The animation control verb, against a stored image and against nothing.
    feed(t, apc("a=a,i=51,s=1"));
    feed(t, apc("a=a,i=51,s=2"));
    feed(t, apc("a=a,i=999,s=1"));
    CHECK_MSG(liveImageCount(t) == 1, "driving the animation neither adds nor drops an image");
}

TEST(testAChunkedTransferAssemblesAcrossBlocks)
{
    // A payload larger than one APC block arrives as `m=1` continuations, so
    // the terminal carries state between blocks. A transfer that never sends
    // its final block has to leave that state recoverable rather than wedge the
    // parser against every later transfer.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::string whole = base64(rgbaPixels(4, 2));
    const std::size_t half = whole.size() / 2;

    feed(t, apc("a=t,f=32,s=4,v=2,i=61,m=1", whole.substr(0, half)));
    CHECK_MSG(liveImageCount(t) == 0, "an unfinished transfer stores nothing");
    feed(t, apc("m=0", whole.substr(half)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 61) != nullptr, "the final block completes it");

    // A transfer left open does not capture the next transfer: that block
    // carries the keys only a first block carries, so it starts fresh and the
    // abandoned data is dropped (test_kitty_abandoned_transfer has the cases).
    const int beforeAbandon = liveImageCount(t);
    feed(t, apc("a=t,f=32,s=4,v=2,i=62,m=1", whole.substr(0, half)));
    store(t, 63);
    CHECK_MSG(imageWithID(t, 63) != nullptr, "the next transfer lands");
    CHECK_MSG(imageWithID(t, 62) == nullptr, "the abandoned one stores nothing");
    CHECK_MSG(liveImageCount(t) == beforeAbandon + 1, "and nothing already stored was lost");

    store(t, 64);
    CHECK_MSG(imageWithID(t, 64) != nullptr, "the transfer after it lands");
}

namespace {

/// Stores `id` and places its anchor at the ZERO-based cell (column, row).
///
/// The delete verbs address cells one-based, as cursor positions are, so a
/// placement anchored here at (2, 1) is selected by `x=3,y=2`. Writing the
/// fixture in the keys' own numbering would let an off-by-one in the selector
/// agree with an off-by-one in the case, and both would pass — which is what
/// happened before the two numberings were separated.
void placeAt(Ubiquitty::Terminal& t, std::uint32_t id, int column, int row,
    int columns = 1, int rows = 1, const std::string& extra = "")
{
    store(t, id);
    feed(t, "\033[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H");
    feed(t, apc("a=p,i=" + std::to_string(id) + ",c=" + std::to_string(columns) + ",r=" + std::to_string(rows) + (extra.empty() ? "" : "," + extra)));
}

} // namespace

// The geometry selectors. Each picks placements by where they sit rather than by
// which image they draw, and `x`/`y` carry CELL coordinates for these modes —
// the same keys mean the source rectangle's origin on a placement command, so
// reading them the other way here would delete by a number the client meant as
// a pixel offset.
TEST(testDeletingByCellSelectsThePlacementCoveringIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    placeAt(t, 101, 2, 1, 3, 2); // covers columns 2..4, rows 1..2
    placeAt(t, 102, 8, 5, 1, 1);
    CHECK(placementCount(t) == 2);

    // A cell inside the first placement takes it and leaves the other.
    feed(t, apc("a=d,d=p,x=4,y=3"));
    CHECK_MSG(placementCount(t) == 1, "the covering placement is gone");
    CHECK_MSG(liveImageCount(t) == 2, "and the lower-case mode kept both images");

    // A cell inside nothing takes nothing.
    feed(t, apc("a=d,d=p,x=16,y=10"));
    CHECK_MSG(placementCount(t) == 1, "a cell no placement covers selects none");

    // The cell one past each far edge, which is the boundary a containment test
    // gets wrong without saying so: a placement one cell wider than it is looks
    // identical from inside and from far away.
    placeAt(t, 103, 2, 1, 3, 2); // columns 2..4, rows 1..2, zero-based
    const int withEdgeProbe = placementCount(t);
    feed(t, apc("a=d,d=p,x=6,y=2")); // column 5 zero-based: one past the right edge
    CHECK_MSG(placementCount(t) == withEdgeProbe, "the cell past the right edge is outside");
    feed(t, apc("a=d,d=p,x=4,y=4")); // row 3 zero-based: one past the bottom edge
    CHECK_MSG(placementCount(t) == withEdgeProbe, "the cell past the bottom edge is outside");

    // And the last cell each edge does cover, so the bound is measured from
    // both sides rather than only from outside.
    feed(t, apc("a=d,d=p,x=5,y=3")); // column 4, row 2 zero-based: the far corner
    CHECK_MSG(placementCount(t) < withEdgeProbe, "the far corner is inside");

    // Upper case frees the image whose last placement went, and leaves the
    // images whose placements the selector did not reach.
    const int imagesBeforeUpperCase = liveImageCount(t);
    feed(t, apc("a=d,d=P,x=9,y=6"));
    CHECK_MSG(placementCount(t) == 0, "the remaining placement is gone");
    CHECK_MSG(liveImageCount(t) == imagesBeforeUpperCase - 1,
        "the upper-case mode freed exactly the image it selected");
}

TEST(testDeletingByColumnAndRowSelectsTheStripThroughThem)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    placeAt(t, 111, 2, 1, 3, 1); // columns 2..4 on row 1
    placeAt(t, 112, 2, 6, 1, 1); // column 2 on row 6
    placeAt(t, 113, 9, 1, 1, 1); // column 9 on row 1
    CHECK(placementCount(t) == 3);

    // A column takes every placement crossing it, whatever its row.
    feed(t, apc("a=d,d=x,x=3"));
    CHECK_MSG(placementCount(t) == 1, "both placements over column 2 are gone");

    // A row takes every placement crossing it, whatever its column.
    feed(t, apc("a=d,d=y,y=2"));
    CHECK_MSG(placementCount(t) == 0, "the placement on row 1 is gone");
    CHECK_MSG(liveImageCount(t) == 3, "and the lower-case modes kept every image");

    // A column and a row nothing sits on select nothing.
    placeAt(t, 114, 4, 4, 1, 1);
    feed(t, apc("a=d,d=x,x=20"));
    feed(t, apc("a=d,d=y,y=10"));
    CHECK_MSG(placementCount(t) == 1, "an empty column and row select nothing");
}

TEST(testDeletingByZIndexSelectsTheLayer)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    placeAt(t, 121, 2, 1, 1, 1, "z=5");
    placeAt(t, 122, 4, 1, 1, 1, "z=5");
    placeAt(t, 123, 6, 1, 1, 1, "z=9");
    CHECK(placementCount(t) == 3);

    feed(t, apc("a=d,d=z,z=5"));
    CHECK_MSG(placementCount(t) == 1, "both placements on that layer are gone");

    // A layer nothing sits on selects nothing, and a delete with no z at all
    // must not take every placement by matching a default.
    feed(t, apc("a=d,d=z,z=42"));
    CHECK(placementCount(t) == 1);
    feed(t, apc("a=d,d=z"));
    CHECK_MSG(placementCount(t) == 1, "no z names no layer rather than all of them");

    feed(t, apc("a=d,d=Z,z=9"));
    CHECK_MSG(placementCount(t) == 0, "the named layer is gone");
}

TEST(testDeletingByCellAndLayerNeedsBoth)
{
    // `q` is the cell selector narrowed by a layer, so a placement at the cell
    // on another layer stays.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    placeAt(t, 131, 3, 2, 1, 1, "z=1");
    placeAt(t, 132, 3, 2, 1, 1, "z=7");
    CHECK(placementCount(t) == 2);

    feed(t, apc("a=d,d=q,x=4,y=3,z=1"));
    CHECK_MSG(placementCount(t) == 1, "only the placement on that layer went");

    feed(t, apc("a=d,d=q,x=4,y=3,z=7"));
    CHECK_MSG(placementCount(t) == 0, "and the other goes with its own layer named");
}

TEST(testDeletingUnderTheCursorReadsTheCursorRatherThanAKey)
{
    // `c` carries no coordinates: the cursor's own cell is the selector, so a
    // client deletes what it is sitting on without computing where that is.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    placeAt(t, 141, 5, 3, 2, 2); // covers columns 5..6, rows 3..4
    placeAt(t, 142, 1, 1, 1, 1);
    CHECK(placementCount(t) == 2);

    // Park the cursor inside the first placement.
    feed(t, "\033[4;6H");
    feed(t, apc("a=d,d=c"));
    CHECK_MSG(placementCount(t) == 1, "the placement under the cursor is gone");
    CHECK_MSG(liveImageCount(t) == 2, "and its image is kept");

    // Somewhere neither covers.
    feed(t, "\033[9;19H");
    feed(t, apc("a=d,d=c"));
    CHECK_MSG(placementCount(t) == 1, "a cursor over nothing selects nothing");

    feed(t, "\033[2;2H");
    feed(t, apc("a=d,d=C"));
    CHECK_MSG(placementCount(t) == 0, "the upper-case form takes it too");
    CHECK_MSG(liveImageCount(t) < 2, "and frees the image behind it");
}

TEST(testDeletingByIdRangeTakesTheWholeInterval)
{
    // `r` selects an inclusive id range, which `x` and `y` carry for this mode.
    // A client tearing down a batch it allocated contiguously sends one of
    // these rather than one delete per id.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    for (std::uint32_t id : { 201u, 202u, 203u, 210u }) {
        store(t, id);
        feed(t, apc("a=p,i=" + std::to_string(id) + ",c=1,r=1"));
    }
    CHECK(liveImageCount(t) == 4);
    CHECK(placementCount(t) == 4);

    feed(t, apc("a=d,d=r,x=201,y=203"));
    CHECK_MSG(placementCount(t) == 1, "the range's placements are gone");
    CHECK_MSG(liveImageCount(t) == 4, "and the lower-case mode kept the pixels");

    feed(t, apc("a=d,d=R,x=201,y=203"));
    CHECK_MSG(liveImageCount(t) == 1, "the upper-case mode frees the range");
    CHECK_MSG(imageWithID(t, 210) != nullptr, "and leaves the id outside it alone");

    // A range naming one id is that id, and an empty range takes nothing.
    feed(t, apc("a=d,d=R,x=210"));
    CHECK_MSG(imageWithID(t, 210) == nullptr, "a range with one endpoint is that id");
    store(t, 220);
    feed(t, apc("a=d,d=R,x=300,y=400"));
    CHECK_MSG(imageWithID(t, 220) != nullptr, "a range covering nothing frees nothing");
}

TEST(testDeletingFramesKeepsTheImageAndItsFirstFrame)
{
    // `f` drops the animation and leaves the still. Frame zero aliases the
    // image's own pixel buffer, so freeing it here would leave the slot holding
    // released memory — the image has to survive with one frame.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    store(t, 151, 2, 2);
    feed(t, apc("a=f,f=32,s=2,v=2,i=151", base64(rgbaPixels(2, 2))));
    feed(t, apc("a=f,f=32,s=2,v=2,i=151", base64(rgbaPixels(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* before = imageWithID(t, 151);
    CHECK(before != nullptr);
    CHECK_MSG(before->frameCount >= 1, "the image carries its frames");

    feed(t, apc("a=d,d=f,i=151"));
    const Ubiquitty::Image::TerminalImage* after = imageWithID(t, 151);
    CHECK_MSG(after != nullptr, "the image survives");
    CHECK_MSG(after->pixels != nullptr, "with its pixels");
    CHECK_MSG(after->frameCount == 1, "and exactly the first frame");
    CHECK_MSG(after->currentFrame == 0, "rewound to it");

    // An id nothing holds answers rather than reaching into a null image.
    feed(t, apc("a=d,d=f,i=999"));
    CHECK(imageWithID(t, 151) != nullptr);
}

// Displaying an image (`a=T`) moves the cursor past the rows the image
// occupies. A client that sends pixel dimensions with no c/r leaves the
// control's OWN r at zero — completeCellBox finishes a half-stated cell box, it
// does not invent a whole one — so an advance by that r leaves the cursor where
// it was while the image occupies real rows, and whatever comes next, a Sixel
// beside it included, is drawn over it.
//
// The advance lands on the image's LAST row (rows - 1), so the emitter's
// trailing newline steps just clear of it.
TEST(testDisplayingAnImageMovesTheCursorPastItsRows)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 10;
    t.cellPixelHeight = 20;

    // 60px tall over 20px cells is three rows, and no cell box is stated — the
    // case an advance by the control's r gets wrong, and the one a normal
    // client produces.
    const int before = t.screens.active->cursorY;
    feed(t, apc("a=T,f=32,s=4,v=60,i=70", base64(rgbaPixels(4, 60))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(t.screens.active->cursorY == before + 2,
        "an image with no stated cell box still advances the cursor by the rows it occupies");
}

TEST(testAStatedRowCountIsWhatTheCursorClears)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 10;
    t.cellPixelHeight = 20;

    // r=4 overrides the natural three rows, so the stated box governs and the
    // test cannot pass by accident off the pixel height.
    const int before = t.screens.active->cursorY;
    feed(t, apc("a=T,f=32,s=4,v=60,r=4,i=71", base64(rgbaPixels(4, 60))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(t.screens.active->cursorY == before + 3,
        "a stated r governs the advance, not the image's natural height");
}

TEST(testCursorPolicyOneLeavesTheCursorWhereItWas)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 10;
    t.cellPixelHeight = 20;

    // C=1 is the protocol's "do not move the cursor", and computing a row count
    // must not start moving it.
    const int before = t.screens.active->cursorY;
    feed(t, apc("a=T,f=32,s=4,v=60,C=1,i=72", base64(rgbaPixels(4, 60))));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(t.screens.active->cursorY == before,
        "C=1 pins the cursor even though the image occupies rows");
}

UBIQUITTY_TEST_MAIN()
