// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_terminal_snapshot.cpp — the deterministic terminal-to-image seam
// (Core/Rendering/TerminalSnapshot.cpp): the RGBA composition, the PNG encode
// layered on it, the invalid-input refusal, the padding inset, and the two
// composites that draw over the grid raster — OSC 133 prompt marks in the
// padding gutter and the Tektronix beam overlay.
//
// Assertions are structural (dimensions, emptiness, ink present or absent,
// buffers differing), never golden pixels: the glyph shapes belong to Unifont
// and the geometry helpers have their own tests.

#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Tek/BeamRaster.h"
#include "Core/Tek/Project.h"
#include "Core/Tek/Tek4014.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

using Ubiquitty::Rendering::renderTerminalSnapshotPNG;
using Ubiquitty::Rendering::renderTerminalSnapshotRGBA;
using Ubiquitty::Rendering::TerminalSnapshotOptions;
using UbiquittyTest::feed;

namespace {

// Pixels that differ from the top-left corner's color — a proxy for "ink"
// that stays valid whatever the theme's background is.
int pixelsUnlikeCorner(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    int count = 0;
    for (int index = 0; index < width * height; ++index) {
        const std::size_t offset = static_cast<std::size_t>(index) * 4;
        if (rgba[offset] != rgba[0] || rgba[offset + 1] != rgba[1] || rgba[offset + 2] != rgba[2])
            count++;
    }
    return count;
}

} // namespace

TEST(testSnapshotRendersTextThenPNG)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);

    // An empty grid still renders: full-size image, no ink.
    TerminalSnapshotOptions options;
    options.drawCursor = false;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> empty = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!empty.empty());
    CHECK(width > 0 && height > 0);
    CHECK_EQ(static_cast<int>(empty.size()), width * height * 4);
    CHECK_EQ(pixelsUnlikeCorner(empty, width, height), 0);

    // Text lands as ink.
    feed(t, "##########");
    int inkWidth = 0;
    int inkHeight = 0;
    const std::vector<std::uint8_t> inked = renderTerminalSnapshotRGBA(t, options, inkWidth, inkHeight);
    CHECK(inkWidth == width && inkHeight == height);
    CHECK(pixelsUnlikeCorner(inked, inkWidth, inkHeight) > 0);

    // The PNG entry is the same composition plus an encode: non-empty, and it
    // starts with the PNG signature.
    const std::vector<std::uint8_t> png = renderTerminalSnapshotPNG(t, options);
    CHECK(png.size() > 8);
    CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}

// A cell can carry ten decoration rules at once — framed box four, double
// underline two, ideogram double underline two, strike, overline — and every
// one of them is reachable through SGR together. A renderer that guesses a
// per-cell capacity and clamps to it draws the rows it reaches and drops the
// rest, silently: the picture is wrong in a way nothing reports.
//
// So: fill the grid with cells wearing all of them, and require the ink to be
// the same as when the buffers were large enough from the start. The second
// render reuses the renderer's now-grown buffers, which is the frame after the
// growth; the first is the frame that grows.
TEST(testSnapshotKeepsEveryDecorationRule)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);

    TerminalSnapshotOptions options;
    options.drawCursor = false;

    // SGR 4:2 double underline, 9 strike, 53 overline, 51 framed, 60 ideogram
    // underline: ten rules on every cell, all reachable together. A renderer
    // that guesses a per-cell capacity and clamps to it runs out partway down
    // the grid, so the rows it reaches last are the ones drawn without their
    // rules — the picture is wrong, and nothing reports it.
    // Decorations are emitted per RUN, so a row of identical cells is a
    // handful of rules however many attributes it wears. Alternating the
    // underline style every cell makes each cell its own run, which is what a
    // screen of mixed markup looks like and what actually asks the builder for
    // more rules than a per-cell guess reserves.
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 20; ++column) {
            feed(t, (column % 2) == 0 ? "\x1b[4:2;9;53;51;60m" : "\x1b[4:1;9;53;51;60m");
            feed(t, "W");
        }
    }

    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> frame = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!frame.empty());

    // Every row carries the same content, so every row owes the same ink. What
    // a clamp produces is a top that is inked and a bottom that is not.
    const auto rowBandInk = [&](int firstRow, int lastRow) {
        int count = 0;
        for (int y = firstRow; y < lastRow && y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
                if (at + 3 < frame.size()
                    && (frame[at] != frame[0] || frame[at + 1] != frame[1] || frame[at + 2] != frame[2]))
                    ++count;
            }
        }
        return count;
    };
    const int band = height / 6;
    const int topInk = rowBandInk(0, band);
    const int bottomInk = rowBandInk(height - band, height);
    CHECK(topInk > 0 && bottomInk > 0);
    // Every row carries the same markup, so every row owes the same ink. A
    // clamp keeps whichever rules the builder emitted before it ran out, which
    // leaves the bands visibly unlike each other — the picture is wrong in a
    // way nothing reports.
    const int larger = topInk > bottomInk ? topInk : bottomInk;
    const int smaller = topInk > bottomInk ? bottomInk : topInk;
    CHECK_MSG(larger * 4 <= smaller * 5,
        "identical rows rendered with unlike amounts of ink, which is a decoration capacity that ran out");
}

TEST(testSnapshotRefusesInvalidTerminal)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.columns = 0; // degenerate: the guard must answer before any capture

    TerminalSnapshotOptions options;
    int width = 7;
    int height = 7;
    CHECK(renderTerminalSnapshotRGBA(t, options, width, height).empty());
    CHECK_EQ(width, 0); // both dimensions are reset, not left stale
    CHECK_EQ(height, 0);
    CHECK(renderTerminalSnapshotPNG(t, options).empty());
}

// Every arm hands the frame its own point space, so the cursor's thin shapes
// come out one geometry across the arms. A scale or a cell the caller does not
// have leaves the options as they were.
TEST(testSnapshotPointSpaceFollowsTheBackingScale)
{
    TerminalSnapshotOptions options;
    Ubiquitty::Rendering::applyPointSpace(options, 38.0f, 2.0, 3.0);
    CHECK_EQ(options.cellHeightPoints, 19.0);
    CHECK_EQ(options.lineSpacingPoints, 3.0);

    Ubiquitty::Rendering::applyPointSpace(options, 38.0f, 0.0, 5.0);
    CHECK_EQ(options.cellHeightPoints, 19.0); // no scale: untouched
    CHECK_EQ(options.lineSpacingPoints, 3.0);

    Ubiquitty::Rendering::applyPointSpace(options, 0.0f, 2.0, 5.0);
    CHECK_EQ(options.cellHeightPoints, 19.0); // no cell: untouched
    CHECK_EQ(options.lineSpacingPoints, 3.0);

    Ubiquitty::Rendering::applyPointSpace(options, 16.0f, 1.0, 0.0);
    CHECK_EQ(options.cellHeightPoints, 16.0);
    CHECK_EQ(options.lineSpacingPoints, 0.0);
}

TEST(testSnapshotPaddingInsetsTheGrid)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);

    TerminalSnapshotOptions bare;
    bare.drawCursor = false;
    int gridWidth = 0;
    int gridHeight = 0;
    CHECK(!renderTerminalSnapshotRGBA(t, bare, gridWidth, gridHeight).empty());

    TerminalSnapshotOptions padded;
    padded.drawCursor = false;
    padded.style.paddingInPixels = { .left = 8.0f, .right = 6.0f, .top = 4.0f, .bottom = 2.0f };
    int width = 0;
    int height = 0;
    CHECK(!renderTerminalSnapshotRGBA(t, padded, width, height).empty());
    CHECK_EQ(width, gridWidth + 8 + 6);
    CHECK_EQ(height, gridHeight + 4 + 2);
}

TEST(testSnapshotCharacterSpacingChangesTheCellPitch)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 12);
    feed(terminal, "spaced text");

    TerminalSnapshotOptions options;
    options.drawCursor = false;
    int plainWidth = 0;
    int plainHeight = 0;
    CHECK(!renderTerminalSnapshotRGBA(terminal, options, plainWidth, plainHeight).empty());
    for (int spacing : { -2, 3 }) {
        options.style.characterSpacingInPixels = spacing;
        int width = 0;
        int height = 0;
        CHECK(!renderTerminalSnapshotRGBA(terminal, options, width, height).empty());
        CHECK_EQ(width, plainWidth + terminal.columns * spacing);
        CHECK_EQ(height, plainHeight);
    }
}

TEST(testSnapshotOtherMatchPillReachesIntoPadding)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 8);
    feed(terminal, "ab");

    TerminalSnapshotOptions options;
    options.drawCursor = false;
    options.style.unifontScale = 1;
    options.style.paddingInPixels = { .left = 8.0f };
    int width = 0;
    int height = 0;
    const auto plain = renderTerminalSnapshotRGBA(terminal, options, width, height);
    CHECK(!plain.empty());

    const Ubiquitty::Rendering::SelectionRowSpan match { 0, 0, 1 };
    options.otherSearchMatchSpans = &match;
    options.otherSearchMatchSpanCount = 1;
    const auto searched = renderTerminalSnapshotRGBA(terminal, options, width, height);
    CHECK_EQ(searched.size(), plain.size());
    const auto differsAt = [&](int x, int y) {
        const auto offset = static_cast<std::size_t>(y * width + x) * 4;
        return searched[offset] != plain[offset] || searched[offset + 1] != plain[offset + 1]
            || searched[offset + 2] != plain[offset + 2];
    };
    CHECK(differsAt(7, 8));
    CHECK(!differsAt(0, 8));
    CHECK(!differsAt(7, 24));
}

// The grid's layers end at the grid box: a curly underline on the last row
// whose wave dips below the cell, and the block cursor beside it, leave the
// padding exactly the frame's ground. Only the overlays reach into it, which
// testSnapshotOtherMatchPillReachesIntoPadding holds.
TEST(testSnapshotGridInkStaysInsideTheGridBox)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 12);
    feed(terminal, "\033[3;1H\033[4:3mwvwvwvwvwvw\033[0m");

    TerminalSnapshotOptions options;
    options.style.unifontScale = 2;
    constexpr int padding = 12;
    options.style.paddingInPixels = { .left = padding, .right = padding, .top = padding, .bottom = padding };
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> rgba = renderTerminalSnapshotRGBA(terminal, options, width, height);
    REQUIRE(!rgba.empty());
    const Ubiquitty::Rendering::RGB8 ground = Ubiquitty::Rendering::resolvePaletteColor(terminal.palette, Ubiquitty::colorDefault, false);
    int inkedPaddingPixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x >= padding && x < width - padding && y >= padding && y < height - padding)
                continue;
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            inkedPaddingPixels += rgba[offset] != ground.red || rgba[offset + 1] != ground.green || rgba[offset + 2] != ground.blue;
        }
    }
    CHECK_EQ(inkedPaddingPixels, 0);
}

// DECSCNM swaps the frame's default ground, and the padding is part of the
// frame: the margins take the configured foreground under reverse video, as
// they do on the arms. A padding filled from the unswapped default leaves a
// reversed screen sitting in an unreversed border, which is the picture the
// native arms were waived against.
TEST(testSnapshotPaddingFollowsReverseVideo)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 12);

    TerminalSnapshotOptions padded;
    padded.drawCursor = false;
    padded.style.paddingInPixels = { .left = 8.0f, .right = 6.0f, .top = 4.0f, .bottom = 2.0f };
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> plain = renderTerminalSnapshotRGBA(t, padded, width, height);
    CHECK(!plain.empty());
    const Ubiquitty::Rendering::RGB8 ground = Ubiquitty::Rendering::resolvePaletteColor(t.palette, Ubiquitty::colorDefault, false);
    CHECK(plain[0] == ground.red && plain[1] == ground.green && plain[2] == ground.blue);

    feed(t, "\033[?5h");
    const std::vector<std::uint8_t> reversed = renderTerminalSnapshotRGBA(t, padded, width, height);
    CHECK(!reversed.empty());
    const Ubiquitty::Rendering::RGB8 ink = Ubiquitty::Rendering::resolvePaletteColor(t.palette, Ubiquitty::colorDefault, true);
    CHECK_MSG(reversed[0] == ink.red && reversed[1] == ink.green && reversed[2] == ink.blue,
        "the top-left corner, pure padding, takes the configured foreground under DECSCNM");
    // The bottom-right corner is padding too, and the swap reaches it alike.
    const std::size_t last = (static_cast<std::size_t>(width) * height - 1) * 4;
    CHECK(reversed[last] == ink.red && reversed[last + 1] == ink.green && reversed[last + 2] == ink.blue);
}

TEST(testSnapshotPromptMarksInkTheGutter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);

    // Two full OSC 133 command cycles: one succeeding, one failing — the
    // executed mark is what the composite draws, the failure picks its color.
    feed(t, "\033]133;A\007ok$ echo\r\n\033]133;C\007out\r\n\033]133;D;0\007");
    feed(t, "\033]133;A\007no$ false\r\n\033]133;C\007\r\n\033]133;D;2\007");

    // The gutter is the left padding; without it the composite declines.
    TerminalSnapshotOptions options;
    options.drawCursor = false;
    options.style.paddingInPixels = { .left = 12.0f };
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> rgba = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!rgba.empty());

    // Some gutter pixel carries the bracket's ink: compare against the
    // top-left corner, which is pure padding fill.
    int gutterInk = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < 12; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (rgba[offset] != rgba[0] || rgba[offset + 1] != rgba[1] || rgba[offset + 2] != rgba[2])
                gutterInk++;
        }
    CHECK(gutterInk > 0);
}

TEST(testDefaultSnapshotOptionsDoNotRetainSemanticMarkInk)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 6, 20);
    feed(terminal, "\033]133;A\007$ echo\r\n\033]133;C\007out\r\n\033]133;D;0\007");

    TerminalSnapshotOptions expected {};
    expected.drawCursor = false;
    expected.style.paddingInPixels = { .left = 12.0f };
    int expectedWidth = 0;
    int expectedHeight = 0;
    const auto expectedPixels = renderTerminalSnapshotRGBA(terminal, expected, expectedWidth, expectedHeight);

    // Reused frame storage must not turn an unset semantic color into a mark override.
    alignas(TerminalSnapshotOptions) std::array<std::byte, sizeof(TerminalSnapshotOptions)> storage {};
    auto* options = new (storage.data()) TerminalSnapshotOptions;
    options->style.promptMarkInk = { 0.0f, 1.0f, 0.0f, 1.0f };
    std::destroy_at(options);
    options = new (storage.data()) TerminalSnapshotOptions;
    options->drawCursor = false;
    options->style.paddingInPixels = expected.style.paddingInPixels;
    int width = 0;
    int height = 0;
    const auto pixels = renderTerminalSnapshotRGBA(terminal, *options, width, height);
    std::destroy_at(options);

    CHECK(!pixels.empty());
    CHECK_EQ(width, expectedWidth);
    CHECK_EQ(height, expectedHeight);
    CHECK(pixels == expectedPixels);
}

TEST(testSnapshotTektronixBeamOverlay)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);
    feed(t, "\033[?38h"); // DECSET 38 — enter Tektronix mode
    CHECK(t.isTekModeActive);

    // A dark move then a draw: one Line primitive in the working frame.
    const char move[] = { 0x1D, 0x27, 0x7A, 0x2F, 0x54 }; // GS + (2000,1000)
    feed(t, std::string_view(move, sizeof(move)));
    const char draw[] = { 0x21, 0x72, 0x20, 0x59 }; // (100,200) — pen down
    feed(t, std::string_view(draw, sizeof(draw)));
    CHECK_EQ(Ubiquitty::tekPrimitiveCount(t), 1);

    // A dotted line as well — the composite walks the dash pattern instead of
    // one solid stroke.
    feed(t, "\033a"); // ESC a — dotted line style
    const char dashedMove[] = { 0x1D, 0x27, 0x7A, 0x20, 0x40 }; // GS + (0,1000)
    feed(t, std::string_view(dashedMove, sizeof(dashedMove)));
    const char dashedDraw[] = { 0x22, 0x60, 0x2F, 0x54 }; // pen down across
    feed(t, std::string_view(dashedDraw, sizeof(dashedDraw)));
    CHECK_EQ(Ubiquitty::tekPrimitiveCount(t), 2);

    // A point-plot primitive too, so all beam kinds composite.
    feed(t, "\034"); // FS — point plot
    const char point[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, std::string_view(point, sizeof(point)));
    CHECK_EQ(Ubiquitty::tekPrimitiveCount(t), 3);

    TerminalSnapshotOptions options;
    options.drawCursor = false;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> working = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!working.empty());
    CHECK(pixelsUnlikeCorner(working, width, height) > 10); // the beam is lit

    // ESC FF commits the frame; the composite then reads the committed display
    // list instead of the working one, and the beam stays lit.
    feed(t, "\033\014");
    CHECK_EQ(Ubiquitty::tekDisplayCount(t), 3); // every primitive, committed
    const std::vector<std::uint8_t> committed = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!committed.empty());
    CHECK(pixelsUnlikeCorner(committed, width, height) > 10);
}

// The software arm draws Core's bloom: off the start of a run the halo is lit
// as far past the end as it reaches across, and both reaches stay inside the
// frame for a run along the bottom of the address space.
TEST(testSnapshotTektronixGlowRoundsTheRunEnds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 24);
    feed(t, "\033[?38h\033\014");
    const char chain[] = { 0x1D, 0x20, 0x40, 0x60, 0x7F, 0x38, 0x50, 0x68, 0x40 };
    feed(t, std::string_view(chain, sizeof(chain)));
    CHECK_EQ(Ubiquitty::tekPrimitiveCount(t), 2);

    TerminalSnapshotOptions plain;
    plain.drawCursor = false;
    plain.tekDeviceScale = 2.0f;
    plain.tekColors.mode = Ubiquitty::Tek::ColorMode::Custom;
    plain.tekColors.beam = Ubiquitty::Tek::widenColor(Ubiquitty::ColorRGBA { 0x33, 0xff, 0x66, 255 });
    plain.tekColors.background = Ubiquitty::Tek::widenColor(Ubiquitty::ColorRGBA { 0x0a, 0x0a, 0x0a, 255 });
    TerminalSnapshotOptions glowing = plain;
    glowing.shouldDrawTekGlow = true;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> unlit = renderTerminalSnapshotRGBA(t, plain, width, height);
    const std::vector<std::uint8_t> lit = renderTerminalSnapshotRGBA(t, glowing, width, height);
    CHECK(!unlit.empty() && lit.size() == unlit.size());

    const Ubiquitty::Tek::BeamGeometry beam = Ubiquitty::Tek::beamGeometry(2.0f);
    const Ubiquitty::Tek::Projection projection = Ubiquitty::Tek::makeBeamProjection(Ubiquitty::tekScreenWidth(t),
        Ubiquitty::tekScreenHeight(t), 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 2.0f);
    const Ubiquitty::Tek::DevicePoint start = Ubiquitty::Tek::project(projection, 0, 0);
    const auto green = [&](const std::vector<std::uint8_t>& pixels, float x, float y) {
        const int column = static_cast<int>(x);
        const int row = static_cast<int>(y);
        CHECK(column >= 0 && column < width && row >= 0 && row < height);
        return pixels[(static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)) * 4 + 1];
    };
    const Ubiquitty::Tek::DevicePoint end = Ubiquitty::Tek::project(projection, 3136, 124);
    const float length = std::hypot(end.x - start.x, end.y - start.y);
    const float unitX = (end.x - start.x) / length;
    const float unitY = (end.y - start.y) / length;
    // Off the start, straight back and diagonally, and square to the middle:
    // each pixel is lifted to exactly the kernel at its centre's distance from
    // the run — the kernel the Metal glow evaluates — over the tube's ground.
    const Ubiquitty::Tek::BeamRun run(start.x, start.y, end.x, end.y);
    std::array<Ubiquitty::Tek::HaloLevel, 1024> levels {};
    const int levelCount = Ubiquitty::Tek::fillHaloLevels(beam, 0x33, 0xff, 0x66, levels);
    const float offset = beam.haloReach * 0.4f;
    const float samples[][2] = { { -offset, 0.0f }, { -offset * 0.7071f, offset * 0.7071f }, { length * 0.5f, offset } };
    for (const auto& at : samples) {
        const float x = start.x + unitX * at[0] - unitY * at[1];
        const float y = start.y + unitY * at[0] + unitX * at[1];
        const int index = Ubiquitty::Tek::haloLevelIndex(run.distanceToPixel(static_cast<int>(x), static_cast<int>(y)));
        CHECK(index < levelCount);
        const int expected = std::max<int>(0x0a, levels[static_cast<std::size_t>(index)].green);
        CHECK(green(lit, x, y) > green(unlit, x, y));
        CHECK_EQ(static_cast<int>(green(lit, x, y)), expected);
    }
    CHECK(start.y + beam.haloReach <= static_cast<float>(height));
}

namespace {

// A 10-bit Tek address — high Y, low Y, high X, low X — for a 12-bit position
// on the 4-unit lattice the 10-bit bytes reach.
std::string tekAddress(int x, int y)
{
    const int column = x / 4;
    const int row = y / 4;
    return { static_cast<char>(0x20 + (row >> 5)), static_cast<char>(0x60 + (row & 31)), static_cast<char>(0x20 + (column >> 5)),
        static_cast<char>(0x40 + (column & 31)) };
}

} // namespace

// The beam is an antialiased capsule, not a stepped one-pixel raster: square
// to a shallow vector every column's ink is centred on the true line and is
// the stroke's width deep; past a vector's end the ink rounds on as a cap;
// round a corner it closes; a plotted point is the round spot.
TEST(testSnapshotTektronixBeamIsAnAntialiasedCapsule)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 48);
    feed(t, "\033[?38h\033\014");
    // A shallow vector across the top, an L joint lower left, a point lower right.
    feed(t, "\035" + tekAddress(200, 2600) + tekAddress(3800, 2900));
    feed(t, "\035" + tekAddress(400, 1600) + tekAddress(1600, 1000) + tekAddress(400, 400));
    feed(t, "\034" + tekAddress(3000, 800));
    CHECK_EQ(Ubiquitty::tekPrimitiveCount(t), 4);

    constexpr float scale = 2.0f;
    TerminalSnapshotOptions options;
    options.drawCursor = false;
    options.tekDeviceScale = scale;
    options.tekColors.mode = Ubiquitty::Tek::ColorMode::Custom;
    options.tekColors.beam = Ubiquitty::Tek::widenColor(Ubiquitty::ColorRGBA { 0xff, 0xff, 0xff, 255 });
    options.tekColors.background = Ubiquitty::Tek::widenColor(Ubiquitty::ColorRGBA { 0, 0, 0, 255 });
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> pixels = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!pixels.empty());
    if (pixels.empty())
        return;
    // White over black mixes to the coverage byte itself.
    const auto coverage = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0.0f;
        return static_cast<float>(pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4]) / 255.0f;
    };
    const Ubiquitty::Tek::BeamGeometry beam = Ubiquitty::Tek::beamGeometry(scale);
    const Ubiquitty::Tek::Projection projection = Ubiquitty::Tek::makeBeamProjection(Ubiquitty::tekScreenWidth(t),
        Ubiquitty::tekScreenHeight(t), 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), scale);

    // Square to the shallow vector: ink centred on the line, one stroke deep.
    const Ubiquitty::Tek::DevicePoint start = Ubiquitty::Tek::project(projection, 200, 2600);
    const Ubiquitty::Tek::DevicePoint end = Ubiquitty::Tek::project(projection, 3800, 2900);
    const float slope = (end.y - start.y) / (end.x - start.x);
    const float depth = 2.0f * beam.halfThickness * std::sqrt(1.0f + slope * slope);
    int columns = 0;
    for (int x = static_cast<int>(start.x) + 3; x < static_cast<int>(end.x) - 3; ++x) {
        const float lineY = start.y + (static_cast<float>(x) + 0.5f - start.x) * slope;
        float ink = 0.0f;
        float moment = 0.0f;
        for (int y = static_cast<int>(lineY) - 4; y <= static_cast<int>(lineY) + 4; ++y) {
            ink += coverage(x, y);
            moment += coverage(x, y) * (static_cast<float>(y) + 0.5f);
        }
        CHECK_MSG(ink > 0.0f && std::abs(moment / ink - lineY) <= 0.15f, "the column's ink is centred on the vector, not on a stair step");
        CHECK_MSG(std::abs(ink - depth) <= 0.15f, "the column's ink is the stroke's width deep");
        ++columns;
    }
    CHECK(columns > 100);

    // Past the end the beam rounds on: the half-disc of the cap beyond it.
    const float length = std::hypot(end.x - start.x, end.y - start.y);
    const float unitX = (end.x - start.x) / length;
    const float unitY = (end.y - start.y) / length;
    float capInk = 0.0f;
    float endInk = 0.0f;
    for (int y = static_cast<int>(end.y) - 4; y <= static_cast<int>(end.y) + 4; ++y) {
        for (int x = static_cast<int>(end.x) - 4; x <= static_cast<int>(end.x) + 4; ++x) {
            const float offsetX = static_cast<float>(x) + 0.5f - end.x;
            const float offsetY = static_cast<float>(y) + 0.5f - end.y;
            if (offsetX * unitX + offsetY * unitY > 0.0f)
                capInk += coverage(x, y);
            if (std::hypot(offsetX, offsetY) <= beam.halfThickness - 0.5f + 0.05f)
                endInk = std::max(endInk, coverage(x, y));
        }
    }
    const float halfDisc = 3.14159265f * beam.halfThickness * beam.halfThickness * 0.5f;
    CHECK_MSG(std::abs(capInk - halfDisc) <= 0.4f, "a vector's end is a round cap");
    CHECK_MSG(endInk >= 0.99f || beam.halfThickness < 1.0f, "the endpoint itself is fully lit");

    // The joint closes: every pixel within the core's solid radius of the
    // shared vertex is fully lit, the outside of the corner included.
    const Ubiquitty::Tek::DevicePoint vertex = Ubiquitty::Tek::project(projection, 1600, 1000);
    int jointPixels = 0;
    for (int y = static_cast<int>(vertex.y) - 3; y <= static_cast<int>(vertex.y) + 3; ++y) {
        for (int x = static_cast<int>(vertex.x) - 3; x <= static_cast<int>(vertex.x) + 3; ++x) {
            if (std::hypot(static_cast<float>(x) + 0.5f - vertex.x, static_cast<float>(y) + 0.5f - vertex.y) > beam.halfThickness - 0.5f - 0.05f)
                continue;
            CHECK_MSG(coverage(x, y) >= 0.99f, "the joint has no notch");
            ++jointPixels;
        }
    }
    CHECK(jointPixels >= 1);

    // The point is the round spot: a disc of the beam's radius, centred on it.
    const Ubiquitty::Tek::DevicePoint spot = Ubiquitty::Tek::project(projection, 3000, 800);
    float spotInk = 0.0f;
    float spotX = 0.0f;
    float spotY = 0.0f;
    for (int y = static_cast<int>(spot.y) - 4; y <= static_cast<int>(spot.y) + 4; ++y) {
        for (int x = static_cast<int>(spot.x) - 4; x <= static_cast<int>(spot.x) + 4; ++x) {
            spotInk += coverage(x, y);
            spotX += coverage(x, y) * (static_cast<float>(x) + 0.5f);
            spotY += coverage(x, y) * (static_cast<float>(y) + 0.5f);
        }
    }
    const float disc = 3.14159265f * beam.halfThickness * beam.halfThickness;
    CHECK_MSG(std::abs(spotInk - disc) <= 0.5f, "a point lights the beam's disc");
    CHECK_MSG(spotInk > 0.0f && std::hypot(spotX / spotInk - spot.x, spotY / spotInk - spot.y) <= 0.15f, "centred on the point");
}

TEST(testSnapshotStyledUnderlinesDrawTheirRules)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 24);

    // One run per rule shape: curly, dotted, dashed, plus a strike. Each is a
    // distinct arm of the decoration composite — the CPU port of the GPU's
    // decorationFragment — and skip-ink routes every rule pixel through the
    // atlas coverage taps.
    feed(t, "\033[4:3mcurly\033[0m \033[4:4mdotted\033[0m \033[4:5mdashed\033[0m \033[9mstruck\033[0m");

    TerminalSnapshotOptions plain;
    plain.drawCursor = false;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> undecorated = renderTerminalSnapshotRGBA(t, plain, width, height);
    CHECK(!undecorated.empty());

    TerminalSnapshotOptions inked = plain;
    inked.style.shouldUnderlineSkipInk = true;
    inked.style.underlineSkipInkPaddingInPixels = 1.0f;
    int inkedWidth = 0;
    int inkedHeight = 0;
    const std::vector<std::uint8_t> skipInked = renderTerminalSnapshotRGBA(t, inked, inkedWidth, inkedHeight);
    CHECK(!skipInked.empty());
    CHECK(inkedWidth == width && inkedHeight == height);

    // Both draw ink, and skip-ink changes which pixels the rules light: where
    // a descender crosses a rule, the skipping render must leave a gap the
    // plain one fills.
    CHECK(pixelsUnlikeCorner(undecorated, width, height) > 0);
    CHECK(pixelsUnlikeCorner(skipInked, width, height) > 0);
    CHECK(undecorated != skipInked);
}

TEST(testSnapshotBuilderTiersDrawTheirInk)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 24);

    // The builder tiers no default flow reaches: the ideogram marks (SGR
    // 60-64 — underline, double, overline, double overline, stress), the
    // double and curly underlines, and a dashed box rule.
    feed(t, "\033[60m一二\033[0m \033[61m三\033[0m \033[62m四\033[0m \033[63m五\033[0m \033[64m六\033[0m\r\n");
    feed(t, "\033[21mdd\033[0m \033[4:3mcc\033[0m ┄┄\r\n");
    feed(t, "find-me here");

    TerminalSnapshotOptions plain;
    plain.drawCursor = false;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> base = renderTerminalSnapshotRGBA(t, plain, width, height);
    CHECK(!base.empty());
    CHECK(pixelsUnlikeCorner(base, width, height) > 0);

    // The focused search match swaps the cell's ink for the contrast pair:
    // the highlighted render differs from the plain one exactly because the
    // glyph no longer draws in its own colour.
    Ubiquitty::Rendering::SelectionRowSpan match { 2, 0, 6 };
    TerminalSnapshotOptions searched = plain;
    searched.searchMatchSpans = &match;
    searched.searchMatchSpanCount = 1;
    searched.searchMatchHighlight = { 1.0f, 0.9f, 0.2f, 1.0f };
    const std::vector<std::uint8_t> highlighted = renderTerminalSnapshotRGBA(t, searched, width, height);
    CHECK(!highlighted.empty());
    CHECK(highlighted != base);

    // The DECSET 7402 letterbox: every cell paints the occluding colour and
    // nothing of the text survives. Driven through the mode rather than through
    // the options, because the capture is what resolves it — an option the
    // caller set would be a second channel for the same state.
    feed(t, "\033]7402;fill=808080\033\\\033[?7402h");
    const std::vector<std::uint8_t> letterboxed = renderTerminalSnapshotRGBA(t, plain, width, height);
    CHECK(!letterboxed.empty());
    bool everyPixelOccluded = true;
    for (int index = 0; index < width * height; ++index)
        everyPixelOccluded = everyPixelOccluded && letterboxed[static_cast<std::size_t>(index) * 4] == letterboxed[0];
    CHECK(everyPixelOccluded);
}

TEST(testSnapshotFocusedMatchPillReachesPastItsCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feed(t, "  ab");

    TerminalSnapshotOptions plain;
    plain.drawCursor = false;
    // The bitmap tier, as the portable tool renders it: the scale names the
    // face as Core's bitmap one, and every partial coverage in the frame is
    // then held to the 51 levels the tier is compared at.
    plain.style.unifontScale = 1;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> base = renderTerminalSnapshotRGBA(t, plain, width, height);
    CHECK(!base.empty());
    // GNU Unifont at scale one: an eight by sixteen cell, so every pixel below
    // is addressed in a geometry this test can state.
    CHECK(width == 8 * 8);
    CHECK(height == 4 * 16);
    const auto pixel = [&](const std::vector<std::uint8_t>& rgba, int x, int y) {
        const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
        return std::array<int, 3> { rgba[offset], rgba[offset + 1], rgba[offset + 2] };
    };

    // The focused match on columns 2 and 3 of the first row: cells x = 16 to
    // 32. The pill reaches past them by the corner radius, 3.52 px, so it
    // runs from x = 12.48 to 35.52, and its shadow a further 4 px.
    Ubiquitty::Rendering::SelectionRowSpan match { 0, 2, 3 };
    TerminalSnapshotOptions searched = plain;
    searched.searchMatchSpans = &match;
    searched.searchMatchSpanCount = 1;
    searched.searchMatchHighlight = { 1.0f, 0.9f, 0.2f, 1.0f };
    const std::vector<std::uint8_t> highlighted = renderTerminalSnapshotRGBA(t, searched, width, height);
    CHECK(!highlighted.empty());
    const std::array<int, 3> pill { 255, 230, 51 };
    const std::array<int, 3> ground = pixel(base, 9, 8);

    // Inside the match the cell holds the flat fill; the space beside it at
    // x = 14, mid-row, lies under the pill's end and takes the pill's colour
    // from the ring; x = 10 lies past the end, under its shadow, darker than
    // the cell's own background; x = 2 lies past the shadow and keeps it.
    CHECK(pixel(highlighted, 20, 8) == pill);
    CHECK(pixel(highlighted, 14, 8) == pill);
    const std::array<int, 3> shaded = pixel(highlighted, 10, 8);
    CHECK(shaded != ground);
    CHECK(shaded != pill);
    CHECK(shaded[0] < ground[0] || shaded[1] < ground[1] || shaded[2] < ground[2]);
    CHECK(pixel(highlighted, 2, 8) == ground);
    // The corner: at (13, 0) the pill's arc has passed and only the shadow
    // remains, so the pixel is neither the pill nor the plain ground.
    const std::array<int, 3> corner = pixel(highlighted, 13, 0);
    CHECK(corner != pill);
    CHECK(corner != ground);
}

TEST(testSnapshotHiliteRegionBand)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);
    feed(t, "some text under the band");

    // A valid hilite region with no caller-supplied spans: the render resolves
    // the band itself.
    t.modes.hiliteRegionValid = true;
    t.modes.hiliteStartX = 2;
    t.modes.hiliteStartY = 1;
    t.modes.hiliteFirstRow = 0;
    t.modes.hiliteLastRow = 3;

    TerminalSnapshotOptions options;
    options.drawCursor = false;
    int width = 0;
    int height = 0;
    CHECK(!renderTerminalSnapshotRGBA(t, options, width, height).empty());
    CHECK(width > 0 && height > 0);
}

TEST(theRasterFactsDecideThePhaseGridAndTheTierKeying)
{
    // One fact, three answers: a CoreText sheet floors its pens and keys its
    // rasters per tier, and a bitmap family does neither. Spelled once here so
    // the live capture and the boundary capture — which assemble their options
    // on different threads, from different state — cannot disagree about where
    // a glyph sits.
    TerminalSnapshotOptions options;
    Ubiquitty::Rendering::FrameRasterFacts coreText;
    coreText.subpixelPhaseCount = 3;
    coreText.isCoreTextRaster = true;
    Ubiquitty::Rendering::applyFrameRaster(options, coreText);
    CHECK(options.subpixelPhaseCount == 3);
    CHECK(options.shouldFloorSubpixelPhase);
    CHECK(options.hasRasterTiers);
    CHECK(options.style.unifontScale == 0);

    // The bitmap tier's scale reaches the STYLE, which is what routes the
    // composite: the frontend hands the atlas's verdict over and Core decides
    // with it, rather than the frontend setting a tier flag of its own.
    Ubiquitty::Rendering::FrameRasterFacts bitmap;
    bitmap.subpixelPhaseCount = 1;
    bitmap.isCoreTextRaster = false;
    bitmap.unifontScale = 2;
    Ubiquitty::Rendering::applyFrameRaster(options, bitmap);
    CHECK(options.subpixelPhaseCount == 1);
    CHECK(!options.shouldFloorSubpixelPhase);
    CHECK(!options.hasRasterTiers);
    CHECK(options.style.unifontScale == 2);
}

// The DECSSDT status row is not part of the grid — it is a one-row buffer
// beside it — so it reaches an image only if something asks Core for it. This
// entry takes the whole terminal, so it is the something: a frontend that hands
// it a terminal and options of its own gets the row without naming it. A
// frontend left to ask for itself renders the status line as whatever the grid
// holds in the last row, which is blank, and downstream that is
// indistinguishable from a terminal with no status line configured — a golden
// minted from such a render records the feature's absence under its name.
//
// The control render is the half that makes this an assertion about the status
// row rather than about ink: the same terminal with no status line configured
// must leave the band empty, or a stray cursor or a grid write would satisfy
// the first check on its own.
TEST(testSnapshotDerivesTheStatusRowWithoutBeingAsked)
{
    const auto lastRowInk = [](const std::vector<std::uint8_t>& rgba, int width, int height, int rows) {
        int count = 0;
        for (int y = height - height / rows; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
                if (rgba[offset] != rgba[0] || rgba[offset + 1] != rgba[1] || rgba[offset + 2] != rgba[2])
                    count++;
            }
        }
        return count;
    };

    constexpr int rows = 6;
    TerminalSnapshotOptions options;
    options.drawCursor = false;

    // No status line: the last row is the grid's, and nothing was written there.
    Ubiquitty::Terminal bare;
    Ubiquitty::hardReset(bare);
    Ubiquitty::resize(bare, rows, 20);
    int bareWidth = 0;
    int bareHeight = 0;
    const std::vector<std::uint8_t> withoutStatus = renderTerminalSnapshotRGBA(bare, options, bareWidth, bareHeight);
    CHECK(!withoutStatus.empty());
    CHECK_EQ(lastRowInk(withoutStatus, bareWidth, bareHeight, rows), 0);

    // DECSSDT 2 makes the row host-writable, DECSASD 1 aims writes at it, and
    // DECSASD 0 aims them back at the grid. The text lands in the status
    // buffer, never in the grid, so ink in the band can only have come from the
    // row this entry is expected to derive.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, 20);
    feed(t, "\x1b[2$~\x1b[1$}STATUS\x1b[0$}");
    CHECK_EQ(static_cast<int>(t.statusLineType), 2);

    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> withStatus = renderTerminalSnapshotRGBA(t, options, width, height);
    CHECK(!withStatus.empty());
    CHECK(width == bareWidth && height == bareHeight);
    CHECK_MSG(lastRowInk(withStatus, width, height, rows) > 0,
        "the status row reaches the image without the caller passing its cells");
}

UBIQUITTY_TEST_MAIN()
