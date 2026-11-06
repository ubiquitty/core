// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sixel_lifecycle — a sixel image entering by WIRE survives what a shell
// does next.
//
// test_image_invariants holds the model's middle for every protocol, but its
// cases install images through the store; the sixel ingress — DCS q, an async
// decode job, the completion that anchors a placement — was driven by nothing.
// chafa is the reference emitter here: it prints the image, the shell prints a
// prompt under it, the user types and scrolls. Each case is one of those
// moments, because "the image showed and then vanished" is precisely a
// placement that outlived none of them.

#include "Harness.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <memory>
#include <string_view>

namespace {

void feed(Ubiquitty::Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, std::string_view(bytes));
    Ubiquitty::drainAsyncDecodes(t);
}

int livePlacements(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImagePlacementCount; ++i)
        if (t.images.placements[i].used)
            ++n;
    return n;
}

int liveImages(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used)
            ++n;
    return n;
}

int quadCount(const Ubiquitty::Terminal& t, int viewOffset)
{
    Ubiquitty::Image::ImageQuad q[8];
    return Ubiquitty::Rendering::collectImageQuads(&t, { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, viewOffset, { q, static_cast<std::size_t>(8) });
}

// A 6x6 all-painted sixel: raster 1:1, one color, six `~` columns (each `~`
// covers six vertical pixels).
constexpr const char* smallSixel = "\033P0;0;0q\"1;1;6;6#0;2;100;0;0#0~~~~~~\033\\";

std::unique_ptr<Ubiquitty::Terminal> makeTerm(int rows, int columns)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    Ubiquitty::hardReset(*t);
    return t;
}

} // namespace

// The wire ingress: DCS q through the async decode to one stored image and one
// placement, resolvable as one quad. Everything later leans on this base.
TEST(testASixelOnTheWireBecomesOnePlacedImage)
{
    auto t = makeTerm(6, 20);
    feed(*t, smallSixel);
    CHECK(liveImages(*t) == 1);
    CHECK(livePlacements(*t) == 1);
    CHECK(quadCount(*t, 0) == 1);
    t.reset();
}

TEST(aRetinaSixelKeepsItsRightEdge)
{
    auto terminal = makeTerm(60, 220);
    terminal->cellPixelWidth = 16;
    terminal->cellPixelHeight = 40;
    feed(*terminal, "\033P0;1q\"1;1;3024;1964#1;2;100;0;0!3023?~\033\\");
    REQUIRE(liveImages(*terminal) == 1);
    const auto& image = terminal->images.stored[0];
    CHECK_EQ(image.width, 3024);
    CHECK_EQ(image.height, 1964);
    REQUIRE(image.pixels != nullptr);
    CHECK_EQ(image.pixels[3023 * 4], 255);
    CHECK_EQ(image.pixels[3023 * 4 + 3], 255);
}

TEST(sixelRejectsDimensionsAndAreaBeyondTheSharedImageBudget)
{
    auto terminal = makeTerm(24, 80);
    feed(*terminal, "\033Pq\"1;1;16385;1~\033\\");
    CHECK_EQ(liveImages(*terminal), 0);
    feed(*terminal, "\033Pq\"1;1;8192;8192~\033\\");
    CHECK_EQ(liveImages(*terminal), 0);
}

// What a shell does the moment chafa exits: prompt text under the image, then
// a few typed characters with the erase-to-end a line editor emits per key.
// None of that covers the image, so none of it may take the image with it.
TEST(testASixelImageSurvivesThePromptAndTyping)
{
    auto t = makeTerm(8, 20);
    feed(*t, smallSixel);
    CHECK(quadCount(*t, 0) == 1);

    feed(*t, "\r\nprompt$ ");
    CHECK_MSG(livePlacements(*t) == 1, "after the prompt line");
    CHECK_MSG(quadCount(*t, 0) == 1, "after the prompt line");

    for (const char* keystroke : { "l\033[K", "s\033[K", "\010\033[K" }) {
        feed(*t, keystroke);
        CHECK_MSG(livePlacements(*t) == 1, keystroke);
    }
    CHECK(quadCount(*t, 0) == 1);
    t.reset();
}

// Scrolling moves the placement with its content — never duplicates it, never
// drops it — and a placement that leaves the screen is still there one step
// back into scrollback.
TEST(testASixelImageScrollsWithoutVanishingOrDuplicating)
{
    auto t = makeTerm(6, 20);
    feed(*t, smallSixel);
    feed(*t, "\r\n"); // the shell's own newline after the image
    const int before = livePlacements(*t);
    CHECK(before == 1);

    // Push one line into scrollback.
    feed(*t, "\033[6;1H\n");
    CHECK_MSG(livePlacements(*t) == 1, "scrolled by one");
    // On screen (shifted up) or one step back — but somewhere, exactly once.
    const int onScreen = quadCount(*t, 0);
    const int oneBack = quadCount(*t, 1);
    CHECK_MSG(onScreen + oneBack >= 1, "the image resolves on screen or in scrollback");
    CHECK_MSG(onScreen <= 1 && oneBack <= 1, "and never more than once per view");

    // Scroll it fully off, then look back: retention is the point of the
    // shared store, and eviction here is the vanish-on-scroll bug.
    for (int i = 0; i < 8; ++i)
        feed(*t, "\033[6;1H\n");
    CHECK_MSG(liveImages(*t) == 1, "the image is retained for scrollback");
    CHECK_MSG(livePlacements(*t) == 1, "and so is its placement");
    // Nine scrolls in total (one above, eight here), so nine steps back.
    CHECK_MSG(quadCount(*t, 9) >= 1, "and resolves when scrolled back to");
    t.reset();
}

// The tmux content resync, distilled: capture-pane text cannot carry an image,
// so a refeed that destroys the pane's images on every resync makes them vanish
// at the resync cadence. The keeper carries them across. The
// control arm runs the same refeed with no keeper and asserts the images die
// there, so this case cannot pass vacuously against a refeed that stopped
// clearing.
TEST(testAResyncRefeedKeepsImagesWithTheKeeper)
{
    // Control: the bare refeed eats the image (full-history shape).
    {
        auto t = makeTerm(6, 20);
        feed(*t, smallSixel);
        CHECK(quadCount(*t, 0) == 1);
        Ubiquitty::clearBuffer(t.get());
        feed(*t, "\033[Hre-fed prompt line\r\n");
        CHECK_MSG(quadCount(*t, 0) == 0, "the control refeed really is destructive");
    }
    // The keeper, around the same destructive sequence.
    {
        auto t = makeTerm(6, 20);
        feed(*t, smallSixel);
        CHECK(quadCount(*t, 0) == 1);
        {
            Ubiquitty::Image::ContentKeeper keeper(*t);
            Ubiquitty::clearBuffer(t.get());
            feed(*t, "\033[Hre-fed prompt line\r\n");
            CHECK_MSG(quadCount(*t, 0) == 0, "mid-refeed the tables are blank, as stolen");
            keeper.restoreInto(*t);
        }
        CHECK_MSG(liveImages(*t) == 1, "the image crossed the refeed");
        CHECK_MSG(livePlacements(*t) == 1, "and so did its placement");
        CHECK_MSG(quadCount(*t, 0) == 1, "and it draws again");
    }
    // An unrestored keeper owns what it holds: no leak, no double free. The
    // sanitizer run is the assertion here; the CHECK is that the terminal
    // stays consistent.
    {
        auto t = makeTerm(6, 20);
        feed(*t, smallSixel);
        {
            Ubiquitty::Image::ContentKeeper keeper(*t);
        }
        CHECK(liveImages(*t) == 0);
    }
}

namespace {

// A payload that paints with register 1 and never defines it.
constexpr const char* paintsWithRegisterOne = "\033P0;0;0q\"1;1;6;6#1~~~~~~\033\\";
// A payload that defines register 1 as green and paints with it.
constexpr const char* definesRegisterOneGreen = "\033P0;0;0q\"1;1;6;6#1;2;0;100;0#1~~~~~~\033\\";

// The stored image with the highest use stamp: the one the last DCS made.
const Ubiquitty::Image::TerminalImage* newestImage(const Ubiquitty::Terminal& t)
{
    const Ubiquitty::Image::TerminalImage* newest = nullptr;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        const Ubiquitty::Image::TerminalImage& image = t.images.stored[i];
        if (image.used && (!newest || image.lastUsedSeq > newest->lastUsedSeq))
            newest = &image;
    }
    return newest;
}

bool firstPixelIsGreen(const Ubiquitty::Image::TerminalImage* image)
{
    return image && image->pixels && image->pixels[0] == 0 && image->pixels[1] == 255 && image->pixels[2] == 0;
}

} // namespace

// xterm ?8452: what follows the image continues beside it on its last row,
// and an image at the right edge defers the wrap the way a printed character
// does. Reset, the cursor returns to the image's first column.
TEST(testSixelCursorRightModeLeavesTheCursorBesideTheImage)
{
    auto t = makeTerm(6, 20);
    feed(*t, "\033[3;4H");
    feed(*t, smallSixel);
    CHECK(t->screens.active->cursorY == 2);
    CHECK(t->screens.active->cursorX == 3);

    feed(*t, "\033[?8452h\033[3;4H");
    feed(*t, smallSixel);
    CHECK(t->screens.active->cursorY == 2);
    CHECK(t->screens.active->cursorX == 4);
    CHECK(!t->screens.active->wrapPending);

    feed(*t, "\033[3;20H");
    feed(*t, smallSixel);
    CHECK(t->screens.active->cursorX == 19);
    CHECK(t->screens.active->wrapPending);
    t.reset();
}

// A sixel placed from a pending wrap leaves the wrap pending, so the text
// after it starts on the next line (xterm graphics_sixel.c: set_cur_col
// leaves do_wrap).
TEST(testSixelKeepsAPendingWrap)
{
    auto t = makeTerm(6, 20);
    feed(*t, "\033[1;20HF");
    CHECK(t->screens.active->wrapPending);
    feed(*t, smallSixel);
    CHECK(t->screens.active->wrapPending);
    feed(*t, "G");
    CHECK(t->screens.active->cursorY == 1);
    CHECK(t->screens.active->cursorX == 1);
    t.reset();
}

// xterm ?1070 set (the default): every sixel starts from the DEC palette, so a
// register the previous image defined means nothing to the next.
TEST(testPrivateColourRegistersSeedEachSixelFromTheDefaultPalette)
{
    auto t = makeTerm(6, 20);
    feed(*t, definesRegisterOneGreen);
    CHECK(firstPixelIsGreen(newestImage(*t)));
    feed(*t, paintsWithRegisterOne);
    CHECK(liveImages(*t) == 2);
    CHECK(!firstPixelIsGreen(newestImage(*t)));
    t.reset();
}

// xterm ?1070 reset: the registers are the terminal's, so an encoder that
// defines its colours once keeps them for every image after.
TEST(testSharedColourRegistersCarryADefinitionToTheNextSixel)
{
    auto t = makeTerm(6, 20);
    feed(*t, "\033[?1070l");
    feed(*t, definesRegisterOneGreen);
    feed(*t, paintsWithRegisterOne);
    CHECK(liveImages(*t) == 2);
    CHECK(firstPixelIsGreen(newestImage(*t)));
    // Setting the mode again seeds the next image from the default palette.
    feed(*t, "\033[?1070h");
    feed(*t, paintsWithRegisterOne);
    CHECK(!firstPixelIsGreen(newestImage(*t)));
    t.reset();
}

UBIQUITTY_TEST_MAIN()

namespace {

// A 6x24 sixel: four bands of six `~` columns, so at the 10x20 cell the
// picture spans two rows and the cursor has a row to travel after it.
constexpr const char* twoRowSixel = "\033P0;0;0q\"1;1;6;24#0;2;100;0;0#0~~~~~~-#0~~~~~~-#0~~~~~~-#0~~~~~~\033\\";

std::unique_ptr<Ubiquitty::Terminal> makeMeasuredTerm(int rows, int columns)
{
    auto t = makeTerm(rows, columns);
    t->cellPixelWidth = 10;
    t->cellPixelHeight = 20;
    return t;
}

} // namespace

// An emitter's sixel and the newline the shell prints after it arrive in one
// read. The completion parks the cursor from the DCS origin, so an install
// deferred to the end of the read runs after the newline has moved the cursor
// and throws that motion away: the prompt then prints on the picture.
TEST(testTheNewlineAfterASixelInTheSameReadCountsFromBelowIt)
{
    auto t = makeMeasuredTerm(8, 20);
    Ubiquitty::processBytes(t.get(), std::string_view(std::string(twoRowSixel) + "\r\n"));
    CHECK(livePlacements(*t) == 1);
    CHECK_MSG(t->screens.active->cursorY == 2, "the cursor is below the picture, not on its last row");
    t.reset();
}

// A shell ends its prompt setup with an erase below, with the cursor still on
// the row the sixel left it: the picture's last row, where xterm leaves it.
// That erase must not drop the placement, or the rows the cursor was advanced
// over stay blank and the picture shows and then vanishes.
TEST(testTheShellsEraseBelowAtThePromptKeepsTheImage)
{
    auto t = makeMeasuredTerm(8, 20);
    feed(*t, twoRowSixel);
    CHECK(t->screens.active->cursorY == 1);
    feed(*t, "\r\033[0m\033[J\033[94mprompt\033[39m $ \033[K");
    CHECK_MSG(livePlacements(*t) == 1, "the prompt's erase below leaves the picture");
    CHECK(quadCount(*t, 0) == 1);
    feed(*t, "\033[1J");
    CHECK_MSG(livePlacements(*t) == 1, "an erase above leaves it too");
    feed(*t, "\033[2J");
    CHECK_MSG(livePlacements(*t) == 0, "a whole-screen clear takes it");
    t.reset();
}
