// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Image/Sixel/Decoder.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

using namespace Ubiquitty::Image::Sixel;

enum {
    maxSixelWidth = 64,
    maxSixelHeight = 32
};

/// The one canvas every full-size decode below paints onto, so each case
/// starts from the pixels the case before it left.
static std::array<uint8_t, maxSixelWidth * maxSixelHeight> sharedCanvas;

/// The status one decode returned and the image it described.
struct Decoded {
    DecodeStatus status;
    Image image;
};

/// Decodes a payload onto the shared canvas at its full size.
static Decoded decodeOntoCanvas(std::string_view payload)
{
    Decoded result {};
    result.status = decode(payload, sharedCanvas.data(), maxSixelWidth, maxSixelHeight, &result.image);
    return result;
}

/// The palette index the most recent full-size decode left at a pixel.
static int paletteIndexAt(int column, int row)
{
    return sharedCanvas[row * maxSixelWidth + column];
}

/// Whether a palette register holds exactly this colour.
static bool registerHolds(const Image& image, int index, int red, int green, int blue)
{
    return image.palette[index][0] == red && image.palette[index][1] == green && image.palette[index][2] == blue;
}

/// Empty input yields a zero-by-zero image with a clean palette.
TEST(testEmpty)
{
    const Decoded empty = decodeOntoCanvas("");
    CHECK(empty.status == DecodeStatus::OK);
    CHECK_EQ(empty.image.width, 0);
    CHECK_EQ(empty.image.height, 0);
    CHECK_EQ(empty.image.stride, maxSixelWidth);
    CHECK_MSG(!empty.image.truncated, "an image with nothing in it has nothing cut off");
    // Default palette: index 0 = black (0,0,0); index 2 = DEC red
    // (80% red, 13% green, 13% blue) = (204, 33, 33).
    CHECK_MSG(registerHolds(empty.image, 0, 0, 0, 0), "register 0 starts black");
    CHECK_MSG(registerHolds(empty.image, 2, 204, 33, 33), "register 2 starts as the DEC red");
}

/// Single sextet "~" (0x7E - 0x3F = 0x3F = all six bits) lights up
/// one column of six pixels using the current palette (index 0).
TEST(testSingleSextet)
{
    const Decoded column = decodeOntoCanvas("~");
    CHECK(column.status == DecodeStatus::OK);
    CHECK_EQ(column.image.width, 1);
    CHECK_EQ(column.image.height, 6);
    for (int row = 0; row < 6; ++row)
        CHECK_MSG(paletteIndexAt(0, row) == 0, "every row of the column takes the current register, 0");
}

/// Bit pattern: "B" = 0x42 → 0x03, bits 0 and 1 set, so only the top
/// two pixels of the column are painted. Use color 2 (default red) so
/// we can distinguish painted pixels from the pre-cleared backdrop.
TEST(testBitPattern)
{
    const Decoded topPair = decodeOntoCanvas("#2B");
    CHECK(topPair.status == DecodeStatus::OK);
    CHECK_EQ(topPair.image.width, 1);
    CHECK_EQ(topPair.image.height, 2);
    // The two set bits paint red; the row under them keeps the cleared backdrop.
    const int indexByRow[] = { 2, 2, 0 };
    for (int row = 0; row < 3; ++row)
        CHECK_EQ(paletteIndexAt(0, row), indexByRow[row]);
}

/// "!5~" repeats the all-six-pixels sextet five times → width 5.
TEST(testRepeat)
{
    const Decoded repeated = decodeOntoCanvas("!5~");
    CHECK(repeated.status == DecodeStatus::OK);
    CHECK_EQ(repeated.image.width, 5);
    CHECK_EQ(repeated.image.height, 6);
    for (int column = 0; column < 5; ++column)
        CHECK_MSG(paletteIndexAt(column, 0) == 0, "each repetition paints the top row of its own column");
}

/// "~-~" → first sixel-row, then advance, then a second sixel-row at
/// the start of the next 6-pixel band. Height should reach 12.
TEST(testNextRow)
{
    const Decoded twoBands = decodeOntoCanvas("~-~");
    CHECK(twoBands.status == DecodeStatus::OK);
    CHECK_EQ(twoBands.image.width, 1);
    CHECK_EQ(twoBands.image.height, 12);
    for (int row = 0; row < 12; ++row)
        CHECK_MSG(paletteIndexAt(0, row) == 0, "both bands paint the whole first column");
}

/// "$" rewinds currentX within the current sixel-row so later sextets
/// overdraw. Confirms the rewind by selecting a different palette
/// index for the second pass and checking the bitmap holds that.
TEST(testCarriageReturn)
{
    const Decoded overdrawn = decodeOntoCanvas("~$#5~");
    CHECK(overdrawn.status == DecodeStatus::OK);
    CHECK_EQ(overdrawn.image.width, 1);
    for (int row = 0; row < 6; ++row)
        CHECK_MSG(paletteIndexAt(0, row) == 5, "the pass after the rewind repaints the column in register 5");
}

/// RGB palette definition: "#9;2;100;0;0~" sets palette[9] = pure red.
TEST(testRGBPalette)
{
    const Decoded defined = decodeOntoCanvas("#9;2;100;0;0~");
    CHECK(defined.status == DecodeStatus::OK);
    const uint8_t* const colour = defined.image.palette[9];
    CHECK_EQ(colour[0], 255);
    CHECK_EQ(colour[1], 0);
    CHECK_EQ(colour[2], 0);
    CHECK_MSG(paletteIndexAt(0, 0) == 9, "defining a register also selects it");
}

/// HLS palette definition: "#3;1;0;50;100~" → hue 0° in DEC HLS is
/// pure blue (DEC's hue axis is rotated 240° from standard HSL).
TEST(testHlsPalette)
{
    const Decoded defined = decodeOntoCanvas("#3;1;0;50;100~");
    CHECK(defined.status == DecodeStatus::OK);
    const uint8_t* const colour = defined.image.palette[3];
    CHECK_EQ(colour[0], 0);
    CHECK_EQ(colour[1], 0);
    CHECK_MSG(colour[2] == 255, "DEC hue 0 is blue");
}

/// Raster attributes override the inferred extent.
TEST(testRasterAttributes)
{
    // "1;1;10;20  → aspect 1:1, canvas 10x20. The "~" only paints
    // one column, but the raster attributes declare 10x20.
    const Decoded declared = decodeOntoCanvas("\"1;1;10;20~");
    CHECK(declared.status == DecodeStatus::OK);
    CHECK_EQ(declared.image.width, 10);
    CHECK_EQ(declared.image.height, 20);
}

/// Truncation: ask for 10 sextet columns but supply a 4-wide buffer.
TEST(testTruncation)
{
    constexpr uint16_t bufferSide = 4;
    Image img;
    uint8_t small[bufferSide * bufferSide];
    const DecodeStatus status = decode("!10~", small, bufferSide, bufferSide, &img);
    CHECK(status == DecodeStatus::OK);
    CHECK_MSG(img.truncated, "ten columns cannot fit a four-column buffer");
    CHECK_MSG(img.width <= bufferSide, "the reported width stops at the buffer's");
    CHECK_MSG(img.height <= bufferSide, "and so does the reported height");
}

/// Buffer guard: zero-size dimensions are rejected.
TEST(testBufferGuard)
{
    const struct {
        uint16_t width;
        uint16_t height;
        const char* meaning;
    } degenerate[] = {
        { 0, maxSixelHeight, "a buffer with no columns is refused" },
        { maxSixelWidth, 0, "a buffer with no rows is refused" },
    };
    Image img;
    for (const auto& buffer : degenerate)
        CHECK_MSG(decode("~", sharedCanvas.data(), buffer.width, buffer.height, &img) == DecodeStatus::InvalidBuffer, buffer.meaning);
}

/// "!5#1?ABC" — a repeat introducer immediately followed by a non-sextet
/// byte ('#') must not consume the '#'; the palette-select sequence
/// "#1" must take effect for the following sextets.
TEST(testRepeatThenNonSextet)
{
    const Decoded mixed = decodeOntoCanvas("!5#1?ABC");
    CHECK(mixed.status == DecodeStatus::OK);
    // '?' is 0x3F → all-zero sextet; 'A' = 0x41 → 0x02 (bit 1);
    // 'B' = 0x42 → 0x03 (bits 0+1); 'C' = 0x43 → 0x04 (bit 2).
    // After "?" currentX is 1; after "A" currentX is 2; after "B" currentX is 3;
    // after "C" currentX is 4. Width = 4. Palette in use is index 1.
    CHECK_MSG(mixed.image.width == 4, "the repeat count ends at '#', which still selects register 1");
    // 'A' sets only bit 1, so its column is lit at row 1; 'C' sets only bit
    // 2, so its column is lit at row 2.
    CHECK_MSG(paletteIndexAt(1, 1) == 1, "the A column is lit in register 1");
    CHECK_MSG(paletteIndexAt(3, 2) == 1, "the C column is lit in register 1");
}

/// "!1000000~" — pathological repeat must not burn O(repeat) work.
/// After this returns the truncated flag is set and width is bounded
/// by the canvas; the test asserts decode completes in normal time
/// because the repeat is capped and emitSextet stops at the edge.
TEST(testRepeatDosBound)
{
    const Decoded flood = decodeOntoCanvas("!1000000~");
    CHECK(flood.status == DecodeStatus::OK);
    CHECK_MSG(flood.image.truncated, "a million columns overrun the canvas");
    CHECK_MSG(flood.image.width <= maxSixelWidth, "and the width stops at its edge");
}

// ---- measurePayload -----------------------------------------------------------
//
// decode clears maxWidth * maxHeight twice over, so the canvas it is handed is
// what a sixel costs whatever it draws. These pin the two directions that matter:
// the bound must be small for a small payload, and it must never come in under
// what decode would report, or an image that decoded whole starts truncating.

// One sixel byte encodes six vertical pixels, so a band is six rows tall.
static constexpr int sixelBandHeight = 6;
static constexpr Ubiquitty::IntSize boundsCeiling { maxSixelWidth, maxSixelHeight };

/// A two-pixel sixel asks for a two-pixel canvas, not the ceiling.
TEST(testPayloadBoundsSizesToTheContent)
{
    const Ubiquitty::IntSize bounds = measurePayload("#0;2;100;0;0#0~~", boundsCeiling).canvas;
    CHECK_MSG(bounds.width == 2, "two painted columns");
    CHECK_MSG(bounds.height == sixelBandHeight, "one band tall");
    // The ceiling would have been 2048*2048*2 bytes for those two pixels.
    CHECK_MSG(static_cast<long long>(bounds.width) * bounds.height * 2 < 1024, "canvas is bytes, not megabytes");
}

/// A declared canvas is honoured even when almost nothing paints, because decode
/// reports the declared size and would otherwise flag truncation.
TEST(testPayloadBoundsHonoursTheRasterAttribute)
{
    const Ubiquitty::IntSize bounds = measurePayload("\"1;1;10;20~", boundsCeiling).canvas;
    CHECK_MSG(bounds.width >= 10, "declared width covered");
    CHECK_MSG(bounds.height >= 20, "declared height covered");

    // And the decode through that bound reports what it did before: same image,
    // no truncation. This is the case a content-only bound would have broken.
    Image img;
    // Sized to the ceiling rather than to `bounds`: a buffer sized from the value
    // under test cannot survive a regression in it, and a stack smash reports
    // nothing at all where a failed assertion names the case.
    uint8_t buffer[maxSixelWidth * maxSixelHeight];
    DecodeStatus rv = decode("\"1;1;10;20~", buffer, static_cast<uint16_t>(bounds.width), static_cast<uint16_t>(bounds.height), &img);
    CHECK_MSG(rv == DecodeStatus::OK, "decode at the bound");
    CHECK_MSG(img.width == 10, "raster width preserved");
    CHECK_MSG(img.height == 20, "raster height preserved");
    CHECK_MSG(!img.truncated, "not truncated by its own bound");
}

/// Repeats count toward the width, and stay under the ceiling.
TEST(testPayloadBoundsCountsRepeatsAndClamps)
{
    const Ubiquitty::IntSize repeated = measurePayload("!30~", boundsCeiling).canvas;
    CHECK_MSG(repeated.width == 30, "repeat widens the canvas");

    const Ubiquitty::IntSize huge = measurePayload("!1000000~", boundsCeiling).canvas;
    CHECK_MSG(huge.width == maxSixelWidth, "clamped to the ceiling");
    CHECK_MSG(huge.height <= maxSixelHeight, "height clamped too");
}

/// Bands stack: each '-' starts another six rows.
TEST(testPayloadBoundsCountsBands)
{
    const Ubiquitty::IntSize bounds = measurePayload("~-~-~", boundsCeiling).canvas;
    CHECK_MSG(bounds.height == 3 * sixelBandHeight, "three bands");
    CHECK_MSG(bounds.width == 1, "one column per band");
}

/// A '$' returns to column zero, so the widest line wins rather than the sum.
TEST(testPayloadBoundsMeasuresTheWidestLineNotTheTotal)
{
    const Ubiquitty::IntSize bounds = measurePayload("~~~$~", boundsCeiling).canvas;
    CHECK_MSG(bounds.width == 3, "widest run, not the total");
}

/// An empty payload still yields a canvas decode accepts: it rejects a zero one.
TEST(testPayloadBoundsNeverReturnsZero)
{
    const Ubiquitty::IntSize bounds = measurePayload("", boundsCeiling).canvas;
    CHECK_MSG(bounds.width >= 1, "width at least one");
    CHECK_MSG(bounds.height >= 1, "height at least one");
    Image img;
    uint8_t buffer[maxSixelWidth * maxSixelHeight];
    DecodeStatus rv = decode("", buffer, static_cast<uint16_t>(bounds.width), static_cast<uint16_t>(bounds.height), &img);
    CHECK_MSG(rv == DecodeStatus::OK, "decode accepts the empty bound");
}

// The canvas bounds one run; `$` returns to column zero, so it bounds neither the
// work nor the time. These pin the count the caller budgets against.

/// A `!` run that overruns the canvas costs the room it had, not its full count:
/// the emit loop stops at the edge rather than spinning out the rest.
TEST(testMeasurePayloadCountsOnlyEmittedColumns)
{
    const PayloadMeasure fits = measurePayload("!30~", boundsCeiling);
    CHECK_MSG(fits.emittedColumns == 30, "a run inside the canvas costs its length");

    const PayloadMeasure overruns = measurePayload("!1000000~", boundsCeiling);
    CHECK_MSG(overruns.emittedColumns == maxSixelWidth, "a run past the edge costs the row, not the count");
}

/// `$` returns to column zero, so the same band can be repainted: the count grows
/// with the payload while the canvas stays put. This is the shape that ran for
/// thirteen seconds.
TEST(testMeasurePayloadCountsRepaintsPastTheCanvas)
{
    std::string repainting;
    for (int i = 0; i < 50; ++i)
        repainting += "!64~$";
    const PayloadMeasure measured = measurePayload(repainting, boundsCeiling);
    CHECK_MSG(measured.canvas.width == maxSixelWidth, "one band wide");
    CHECK_MSG(measured.canvas.height == sixelBandHeight, "one band tall");
    // 50 repaints of a 64-column band: work the canvas alone would not reveal.
    CHECK_MSG(measured.emittedColumns == 50 * maxSixelWidth, "every repaint counted");
    CHECK_MSG(measured.emittedColumns > static_cast<long long>(measured.canvas.width) * measured.canvas.height,
        "work exceeds the canvas it paints");
}

/// `-` past the canvas bottom keeps the emit loop running with nothing to write,
/// which is the second shape that reached billions. A per-band count would price
/// only the first band and miss it entirely.
TEST(testMeasurePayloadCountsEmissionsBelowTheCanvas)
{
    std::string descending;
    for (int i = 0; i < 200; ++i)
        descending += "!64~-";
    const PayloadMeasure measured = measurePayload(descending, boundsCeiling);
    CHECK_MSG(measured.canvas.height == maxSixelHeight, "canvas clamped at the ceiling");
    // 200 bands of work over a canvas only maxSixelHeight/6 bands tall.
    CHECK_MSG(measured.emittedColumns == 200 * 64, "work past the canvas still counted");
    CHECK_MSG(measured.emittedColumns > static_cast<long long>(measured.canvas.width) * (maxSixelHeight / sixelBandHeight),
        "work exceeds a full repaint of the canvas");
}

/// What a real encoder emits must stay well inside the budget. libsixel writes one
/// pass per colour present in a band, each pass `!` runs separated by `$` — the
/// same grammar the attack uses, which is why the budget is a repaint count rather
/// than a ban on the shape.
TEST(testMeasurePayloadLeavesRoomForRealEncoderOutput)
{
    // Four bands, sixteen colour passes each: a plausible photo, in miniature.
    std::string encoded = "\"1;1;64;24";
    for (int band = 0; band < 4; ++band) {
        for (int colour = 0; colour < 16; ++colour) {
            encoded += "#";
            encoded += static_cast<char>('0' + (colour % 10));
            encoded += "!64~$";
        }
        encoded += "-";
    }
    const PayloadMeasure measured = measurePayload(encoded, boundsCeiling);
    const long long bands = (measured.canvas.height + sixelBandHeight - 1) / sixelBandHeight;
    const long long repaints = measured.emittedColumns / (static_cast<long long>(measured.canvas.width) * bands);
    CHECK_MSG(repaints <= 16, "sixteen colour passes reads as sixteen repaints");
}

UBIQUITTY_TEST_MAIN()
