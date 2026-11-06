// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Where the cursor stands after an inline image, read the way a program reads
// it: a CPR reply. The kitty graphics protocol (kitty's own handle_put_command
// and screen_handle_graphics_command) and iTerm2 (VT100InlineImageHelper's
// writeBaseCharacter) agree that it ends on the image's last row, one column
// right of the image's last cell, so the next character is printed beside the
// picture rather than on it. Kitty's C=1 leaves it where it was, and a step past
// the right margin wraps to the first column of the next row.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

// The 24x24 four-quadrant PNG the iterm2_image snapshot case sends.
constexpr std::string_view quadrantsPNG = "iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAIAAABvFaqvAAAAL0lEQVR42mO4Y6RPELXOZCCIGEYNGjVo1CC8Bql1XyKItnYyEESjBo0aNGoQXgQAtVDf3/jjW+IAAAAASUVORK5CYII=";

// A 24x24 raw RGB kitty payload: 1728 zero bytes.
std::string kittyQuadrantsPayload()
{
    std::string payload;
    for (int group = 0; group < 24 * 24 * 3 / 3; ++group)
        payload += "AAAA";
    return payload;
}

std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int cellWidth, int cellHeight)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::hardReset(*terminal);
    Ubiquitty::resize(*terminal, 24, 80);
    terminal->cellPixelWidth = static_cast<std::uint16_t>(cellWidth);
    terminal->cellPixelHeight = static_cast<std::uint16_t>(cellHeight);
    return terminal;
}

std::string cursorReport(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::drainAsyncDecodes(terminal);
    std::array<char, 64> reply {};
    (void)drain(terminal, reply);
    feed(terminal, "\033[6n");
    const int length = drain(terminal, reply);
    return length > 0 ? std::string(reply.data(), static_cast<std::size_t>(length)) : std::string();
}

std::string reportAfter(int cellWidth, int cellHeight, std::string_view position, const std::string& image)
{
    auto terminal = makeTerminal(cellWidth, cellHeight);
    feed(*terminal, position);
    feed(*terminal, image);
    return cursorReport(*terminal);
}

std::string kitty(std::string_view keys)
{
    return "\033_Ga=T,f=24,s=24,v=24" + std::string(keys) + ";" + kittyQuadrantsPayload() + "\033\\";
}

std::string iterm2(std::string_view keys)
{
    return "\033]1337;File=inline=1" + std::string(keys) + ":" + std::string(quadrantsPNG) + "\007";
}

} // namespace

// At 2x a JetBrains Mono 10 cell is 12x30 device pixels, so a 24x24 image covers
// two columns of one row from either protocol, and at 1x (6x15) four columns of
// two rows.
TEST(theCursorStandsRightOfAKittyImagesLastCell)
{
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty("")), "\033[2;5R");
    CHECK_STR_EQ(reportAfter(6, 15, "\033[2;3H", kitty("")), "\033[3;7R");
    CHECK_STR_EQ(reportAfter(16, 32, "\033[2;1H", kitty("")), "\033[2;3R");
    // A stated cell box is what the cursor steps past.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty(",c=4,r=3")), "\033[4;7R");
    // One stated axis derives the other from the aspect, rounded up.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty(",c=5")), "\033[3;8R");
    // The X/Y offset counts toward the cells a natural-size image covers.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty(",X=1")), "\033[2;6R");
}

TEST(kittyCursorPolicyOneLeavesTheCursorWhereItWas)
{
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty(",C=1")), "\033[2;3R");
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", kitty(",c=4,r=3,C=1")), "\033[2;3R");
}

TEST(anImageReachingTheRightMarginWrapsTheCursorToTheNextRow)
{
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;79H", kitty("")), "\033[3;1R");
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;79H", iterm2(";width=24px;height=24px")), "\033[3;1R");
}

TEST(anImageRunningPastTheBottomScrollsAndLeavesTheCursorBesideIt)
{
    CHECK_STR_EQ(reportAfter(12, 30, "\033[24;3H", kitty(",c=2,r=3")), "\033[24;5R");
    CHECK_STR_EQ(reportAfter(12, 30, "\033[24;3H", iterm2(";width=2;height=3")), "\033[24;5R");
}

TEST(theCursorStandsRightOfAnITerm2ImagesLastCell)
{
    // Npx is device pixels, so the covered cells follow the cell's device size.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", iterm2(";width=24px;height=24px")), "\033[2;5R");
    CHECK_STR_EQ(reportAfter(6, 15, "\033[2;3H", iterm2(";width=24px;height=24px")), "\033[3;7R");
    CHECK_STR_EQ(reportAfter(16, 32, "\033[2;1H", iterm2(";width=24px;height=24px")), "\033[2;3R");
    // Natural size is the image's own pixels.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", iterm2("")), "\033[2;5R");
    // A box of cells is covered whole, whatever part the fitted picture fills.
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", iterm2(";width=4;height=4")), "\033[5;7R");
    CHECK_STR_EQ(reportAfter(12, 30, "\033[2;3H", iterm2(";width=10%")), "\033[5;11R");
}

// The tail draws the cursor over whatever its cell holds, so the cursor must not
// be on the picture: after either protocol's image, no quad reaches the
// cursor's cell.
TEST(noImageQuadCoversTheCursorCellAfterTheImage)
{
    for (const std::string& image : { kitty(""), iterm2(";width=24px;height=24px"), iterm2("") }) {
        for (const Ubiquitty::IntSize cell : { Ubiquitty::IntSize { 12, 30 }, Ubiquitty::IntSize { 16, 32 }, Ubiquitty::IntSize { 6, 15 } }) {
            auto terminal = makeTerminal(cell.width, cell.height);
            feed(*terminal, "\033[2;1H");
            feed(*terminal, image);
            Ubiquitty::drainAsyncDecodes(*terminal);
            const Ubiquitty::FloatSize cellSize { static_cast<float>(cell.width), static_cast<float>(cell.height) };
            std::array<Ubiquitty::Image::ImageQuad, 2> quads {};
            REQUIRE(Ubiquitty::Rendering::collectImageQuads(terminal.get(), cellSize, {}, 1.0f, 0, quads) == 1);
            CHECK_EQ(quads[0].maxXInPixels - quads[0].minXInPixels, 24.0f);
            CHECK_EQ(quads[0].maxYInPixels - quads[0].minYInPixels, 24.0f);
            const float cursorLeft = static_cast<float>(terminal->screens.active->cursorX) * cellSize.width;
            const float cursorTop = static_cast<float>(terminal->screens.active->cursorY) * cellSize.height;
            const bool isOverCursor = quads[0].minXInPixels < cursorLeft + cellSize.width && quads[0].maxXInPixels > cursorLeft
                && quads[0].minYInPixels < cursorTop + cellSize.height && quads[0].maxYInPixels > cursorTop;
            CHECK(!isOverCursor);
        }
    }
}

UBIQUITTY_TEST_MAIN()
